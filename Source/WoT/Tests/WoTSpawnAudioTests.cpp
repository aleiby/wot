// Spawn-audio specs (spawning in sounded
// very loud). Packaged measurement found every AngrealSpawnPool on the map
// playing its rarity sound at BeginPlay, unattenuated, on the listen host
// (16 on Arena_01, 21 on Battle2_01; output peak 3.2-3.6x full scale). These
// pin the retail rules the fix restores:
//  - the level-startup pool spawn is silent (UE1's LoadMap runs
//    SetInitialState before MatchViewportsToActors spawns the player;
//    a server PlaySound only reaches
//    PawnList players);
//  - every later rarity / pickup / respawn sound is a server PlaySound:
//    replicated, volume 1, the unset radius 1600;
//  - every UE1 voice is Clamp(Volume * (1 - d/R), 0, 1) (the Galaxy audio driver),
//    and a light-carried ambient volume is clamped to 1 by GlobalLighting.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTAmbientSound.h"
#include "../WoTCharacter.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Settings/WoTGameSettings.h"

#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "Misc/App.h"
#include "Sound/SoundSubmix.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Sound/SoundAttenuation.h"

namespace
{
	TArray<AWoTSoundBurst*> LiveMarkers(UWorld* World)
	{
		TArray<AWoTSoundBurst*> Out;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				Out.Add(*It);
			}
		}
		return Out;
	}

	const AWoTSoundBurst* MarkerFor(UWorld* World, FName SubPath)
	{
		for (AWoTSoundBurst* Marker : LiveMarkers(World))
		{
			if (Marker->SoundSubPath == SubPath)
			{
				return Marker;
			}
		}
		return nullptr;
	}

	// The voice gain a UE5 source gets at Distance: component volume x the
	// attenuation's distance evaluation (sphere origin at the world origin).
	float VoiceGain(const USoundAttenuation* Att, float Volume, float Distance)
	{
		return Volume * Att->Attenuation.Evaluate(
			FTransform::Identity, FVector(Distance, 0.0f, 0.0f));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpawnAudioPoolSpec,
	"WoT.Sound.SpawnAudio.SpawnPool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpawnAudioPoolSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("pawn spawned"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	const FTransform Xform(FRotator::ZeroRotator, FVector(500, 0, 100));
	AWoTAngrealSpawnPool* Pool = World->SpawnActorDeferred<AWoTAngrealSpawnPool>(
		AWoTAngrealSpawnPool::StaticClass(), Xform);
	Pool->Pool = { AWoTAngrealInvFireball::StaticClass() };
	Pool->Weight = { 1 };
	Pool->RespawnTime = 0.2f;
	Pool->bRespawn = true;
	Pool->FinishSpawning(Xform);

	// Level startup: the item appears, the pool says nothing.
	TestNotNull(TEXT("startup spawned the pickup"), Pool->PickupItem.Get());
	TestEqual(TEXT("startup spawn announces no rarity sound"),
		Pool->LastAnnouncedSoundForTest, FName());
	TestEqual(TEXT("startup spawn leaves no sound marker"), LiveMarkers(World).Num(), 0);

	// Pickup: Inventory.Touch PlaySound(PickupSound) -> one replicated marker at
	// the item, volume 1, radius 1600.
	Pawn->SetActorLocation(Pool->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	TestNotNull(TEXT("the pawn took the fireball"),
		Pawn->InventoryComponent->SelectedItem.Get());
	if (const AWoTSoundBurst* Pickup = MarkerFor(World, FName(TEXT("WOT/Pickup"))))
	{
		TestEqual(TEXT("pickup sound radius is the unset 1600"), Pickup->Radius, 1600.0f);
		TestEqual(TEXT("pickup sound volume is TransientSoundVolume 1"), Pickup->Volume, 1.0f);
	}
	else
	{
		AddError(TEXT("the pickup played no replicated WOT/Pickup marker"));
	}

	// Respawn (state Waiting -> Active): the common-rarity RespawnA, replicated.
	Pawn->SetActorLocation(FVector(0, 0, 100));
	WoTTest::TickWorld(World, 0.6f);
	TestNotNull(TEXT("the pool respawned"), Pool->PickupItem.Get());
	TestEqual(TEXT("the respawn announces the common rarity sound"),
		Pool->LastAnnouncedSoundForTest, FName(TEXT("Angreal/RespawnA")));
	if (const AWoTSoundBurst* Respawn = MarkerFor(World, FName(TEXT("Angreal/RespawnA"))))
	{
		TestEqual(TEXT("respawn sound radius is the unset 1600"), Respawn->Radius, 1600.0f);
		TestEqual(TEXT("respawn sound volume 1"), Respawn->Volume, 1.0f);
		TestTrue(TEXT("respawn sound plays at the pool"),
			Respawn->GetActorLocation().Equals(Pool->GetActorLocation(), 0.1f));
	}
	else
	{
		AddError(TEXT("the respawn played no replicated Angreal/RespawnA marker"));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// A player standing in the pool when it respawns (the packaged pickup run's
// camper heard two pickup sounds per respawn). Retail spawns the item and
// turns its collision off before anything can touch it (AngrealSpawnPool.uc:
// 69-75; UE1 SpawnActor raises no Touch), then
// CheckTouching hands it over once (:94, :55-62).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpawnAudioCamperSpec,
	"WoT.Sound.SpawnAudio.CamperRespawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpawnAudioCamperSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("pawn spawned"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	const FTransform Xform(FRotator::ZeroRotator, FVector(500, 0, 100));
	AWoTAngrealSpawnPool* Pool = World->SpawnActorDeferred<AWoTAngrealSpawnPool>(
		AWoTAngrealSpawnPool::StaticClass(), Xform);
	Pool->Pool = { AWoTAngrealInvFireball::StaticClass() };
	Pool->Weight = { 1 };
	Pool->RespawnTime = 0.2f;
	Pool->bRespawn = true;
	Pool->FinishSpawning(Xform);

	// Take the first pickup and stay in the pool.
	Pawn->SetActorLocation(Pool->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInventory* Held = Cast<AWoTAngrealInventory>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("the camper holds the first fireball"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Let the first pickup's marker expire so the respawn's sounds stand alone.
	WoTTest::TickWorld(World, 0.15f);
	for (AWoTSoundBurst* Marker : LiveMarkers(World))
	{
		Marker->Destroy();
	}
	Held->CurCharges = 5;

	// Respawn while camping: a same-class duplicate -> one charge steal, one
	// pickup sound, the held item survives.
	WoTTest::TickWorld(World, 0.6f);
	int32 PickupSounds = 0;
	for (AWoTSoundBurst* Marker : LiveMarkers(World))
	{
		PickupSounds += Marker->SoundSubPath == FName(TEXT("WOT/Pickup")) ? 1 : 0;
	}
	TestEqual(TEXT("one pickup sound for the camper's respawn handoff"), PickupSounds, 1);
	TestTrue(TEXT("the held fireball survives the duplicate handoff"), IsValid(Held));
	TestEqual(TEXT("the camper still holds exactly one item"), Inv->Items.Num(), 1);
	if (IsValid(Held))
	{
		TestTrue(TEXT("the duplicate's charges were added once"),
			Held->CurCharges > 5);
	}
	TestNull(TEXT("the pool handed its respawned pickup over"), Pool->PickupItem.Get());

	// Respawn a class the camper does NOT hold: the handoff grants it once
	// (the spawn-time overlap must not grant it first and then let the pool's
	// handoff find it in the camper's own inventory, steal its charges from
	// itself and destroy it).
	for (AWoTSoundBurst* Marker : LiveMarkers(World))
	{
		Marker->Destroy();
	}
	Pool->Pool = { AWoTAngrealInvDart::StaticClass() };
	WoTTest::TickWorld(World, 0.6f);
	PickupSounds = 0;
	for (AWoTSoundBurst* Marker : LiveMarkers(World))
	{
		PickupSounds += Marker->SoundSubPath == FName(TEXT("WOT/Pickup")) ? 1 : 0;
	}
	TestEqual(TEXT("one pickup sound when the camper gains a new class"), PickupSounds, 1);
	AWoTAngrealInvDart* Dart = nullptr;
	for (AWoTInventory* Item : Inv->Items)
	{
		if (AWoTAngrealInvDart* Found = Cast<AWoTAngrealInvDart>(Item))
		{
			Dart = Found;
		}
	}
	TestTrue(TEXT("the camper keeps the newly granted dart"), IsValid(Dart));
	TestEqual(TEXT("the camper holds the fireball and the dart"), Inv->Items.Num(), 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpawnAudioVoiceClampSpec,
	"WoT.Sound.SpawnAudio.VoiceClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpawnAudioVoiceClampSpec::RunTest(const FString&)
{
	constexpr float Radius = 1000.0f;
	// Volume <= 1: the plain linear model, shared with the radius-only cache.
	USoundAttenuation* Plain = WoTSound::LinearAttenuation(Radius);
	TestTrue(TEXT("volume 1 reuses the linear attenuation"),
		WoTSound::LinearAttenuation(Radius, 1.0f) == Plain);
	TestTrue(TEXT("volume 0.5 reuses the linear attenuation"),
		WoTSound::LinearAttenuation(Radius, 0.5f) == Plain);

	// Volume > 1: min(1, V*(1 - d/R)), checked against UE's own evaluation.
	for (const float Volume : {1.4f, 2.6721f})
	{
		USoundAttenuation* Clamp = WoTSound::LinearAttenuation(Radius, Volume);
		if (!TestNotNull(TEXT("clamp attenuation built"), Clamp))
		{
			continue;
		}
		TestTrue(TEXT("clamp curves are distinct from the linear model"), Clamp != Plain);
		TestTrue(TEXT("clamp curves are cached per (radius, volume)"),
			WoTSound::LinearAttenuation(Radius, Volume) == Clamp);
		TestEqual(TEXT("falloff is the UE1 radius"), Clamp->Attenuation.FalloffDistance, Radius);
		for (const float Distance : {0.0f, 100.0f, 250.0f, 285.0f, 400.0f, 626.0f,
			800.0f, 999.0f, 1000.0f, 1500.0f})
		{
			const float Expected = FMath::Clamp(Volume * (1.0f - Distance / Radius), 0.0f, 1.0f);
			TestEqual(FString::Printf(TEXT("V=%.4f d=%.0f gain = Clamp(V*(1-d/R),0,1)"),
				Volume, Distance), VoiceGain(Clamp, Volume, Distance), Expected, 1.0e-3f);
		}
	}
	TestNull(TEXT("zero radius still means no attenuation"),
		WoTSound::LinearAttenuation(0.0f, 1.4f));

	// SubPathOf: the PlayReplicated key of a /Game/WoT/Sounds reference.
	TestEqual(TEXT("sound sub path"),
		WoTSound::SubPathOf(TSoftObjectPtr<USoundBase>(FSoftObjectPath(
			TEXT("/Game/WoT/Sounds/Angreal/RespawnA.RespawnA")))),
		FName(TEXT("Angreal/RespawnA")));
	TestEqual(TEXT("null reference has no sub path"),
		WoTSound::SubPathOf(TSoftObjectPtr<USoundBase>()), FName());
	TestEqual(TEXT("out-of-tree reference has no sub path"),
		WoTSound::SubPathOf(TSoftObjectPtr<USoundBase>(FSoftObjectPath(
			TEXT("/Engine/EngineSounds/Foo.Foo")))), FName());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpawnAudioAmbientClampSpec,
	"WoT.Sound.SpawnAudio.AmbientClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpawnAudioAmbientClampSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
	UWoTAmbientSoundComponent* Loud = NewObject<UWoTAmbientSoundComponent>(Host);
	UWoTAmbientSoundComponent* Lit = NewObject<UWoTAmbientSoundComponent>(Host);
	Loud->RegisterComponent();
	Lit->RegisterComponent();

	// SoundVolume 255, unlit: steady state 2*0.7 = 1.4, voice clamped at 1.
	Loud->ConfigureUE1Ambient(nullptr, 64, 255, 64);
	TestEqual(TEXT("unlit loud loop keeps the 1.4 steady-state volume"),
		Loud->VolumeMultiplier, 1.4f, 1.0e-4f);
	const float R = WoTSound::WorldSoundRadius(64);
	if (TestNotNull(TEXT("loud loop attenuation"), Loud->AttenuationSettings.Get()))
	{
		TestEqual(TEXT("loud loop at the source plays at full scale, not 1.4"),
			VoiceGain(Loud->AttenuationSettings, Loud->VolumeMultiplier, 0.0f), 1.0f, 1.0e-3f);
		TestEqual(TEXT("loud loop at half radius = 1.4 * 0.5"),
			VoiceGain(Loud->AttenuationSettings, Loud->VolumeMultiplier, 0.5f * R),
			0.7f, 1.0e-3f);
	}
	// Listener head underwater: volume x0.9, radius x0.8 -> clamp follows.
	Loud->SetUnderwaterMuffle(true);
	if (TestNotNull(TEXT("muffled attenuation"), Loud->AttenuationSettings.Get()))
	{
		const float Rm = 0.8f * R;
		TestEqual(TEXT("muffled falloff is 0.8 R"),
			Loud->AttenuationSettings->Attenuation.FalloffDistance,
			static_cast<float>(FMath::RoundToInt(Rm)));
		TestEqual(TEXT("muffled loop still caps at full scale"),
			VoiceGain(Loud->AttenuationSettings, Loud->VolumeMultiplier, 0.0f), 1.0f, 1.0e-3f);
		TestEqual(TEXT("muffled loop at half radius = 1.26 * 0.5"),
			VoiceGain(Loud->AttenuationSettings, Loud->VolumeMultiplier,
				0.5f * FMath::RoundToInt(Rm)), 0.63f, 2.0e-3f);
	}

	// Light-carried emitter: GlobalLighting clamps the volume itself to 1.
	Lit->ConfigureUE1Ambient(nullptr, 64, 255, 64, 1.0f, /*bLightCarried*/ true);
	TestEqual(TEXT("light-carried loop volume is clamped to 1"), Lit->VolumeMultiplier, 1.0f,
		1.0e-4f);
	if (TestNotNull(TEXT("lit attenuation"), Lit->AttenuationSettings.Get()))
	{
		TestTrue(TEXT("a volume-1 loop uses the plain linear model"),
			Lit->AttenuationSettings->Attenuation.DistanceAlgorithm
				== EAttenuationDistanceModel::Linear);
		TestEqual(TEXT("lit loop at half radius = 0.5"),
			VoiceGain(Lit->AttenuationSettings, Lit->VolumeMultiplier, 0.5f * R), 0.5f, 1.0e-3f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The Settings "Master Volume" never lowered anything on Windows: ApplyAudio
// wrote FApp's volume multiplier, which the platform message pump overwrites
// every frame with the focus state. It is now each audio device's main-submix
// output volume, which neither the pump nor LoadMap's
// SetTransientPrimaryVolume(1) reset touches.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpawnAudioMasterVolumeSpec,
	"WoT.Sound.SpawnAudio.MasterVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpawnAudioMasterVolumeSpec::RunTest(const FString&)
{
	const float PreviousMaster = FWoTGameSettingsAdapter::GetAppliedMasterVolume();
	const float PreviousApp = FApp::GetVolumeMultiplier();
	struct FProjected
	{
		FAudioDevice* Device = nullptr;
		USoundSubmix* Submix = nullptr;
		float Volume = -1.0f;
	};
	TArray<FProjected> Projected;
	FWoTGameSettingsAdapter::SetMasterVolumeProjectionForTesting(
		[&Projected](FAudioDevice* Device, USoundSubmix* Submix, float Volume)
		{
			Projected.Add({Device, Submix, Volume});
		});
	ON_SCOPE_EXIT
	{
		FWoTGameSettingsAdapter::SetMasterVolumeProjectionForTesting({});
		FWoTGameSettings Restore = FWoTGameSettings::Defaults();
		Restore.MasterVolume = PreviousMaster;
		FWoTGameSettingsAdapter::ApplyAudio(Restore);
		FApp::SetVolumeMultiplier(PreviousApp);
	};

	FWoTGameSettings Settings = FWoTGameSettings::Defaults();
	Settings.MasterVolume = 0.35f;
	FWoTGameSettingsAdapter::ApplyAudio(Settings);
	TestEqual(TEXT("the master volume is applied"),
		FWoTGameSettingsAdapter::GetAppliedMasterVolume(), 0.35f);

	TArray<FAudioDevice*> Devices;
	if (FAudioDeviceManager* Manager = FAudioDeviceManager::Get())
	{
		Manager->IterateOverAllDevices([&Devices](Audio::FDeviceId, FAudioDevice* Device)
		{
			if (Device)
			{
				Devices.Add(Device);
			}
		});
	}
	AddInfo(FString::Printf(TEXT("audio devices: %d"), Devices.Num()));
	TestEqual(TEXT("every device receives one master-volume projection"),
		Projected.Num(), Devices.Num());
	for (const FProjected& Entry : Projected)
	{
		TestTrue(TEXT("the projection targets a live device"), Devices.Contains(Entry.Device));
		TestTrue(TEXT("the projection targets that device's main submix"),
			Entry.Device && Entry.Submix == &Entry.Device->GetMainSubmixObject());
		TestEqual(TEXT("the projected main-submix volume is the master volume"),
			Entry.Volume, 0.35f);
	}

	// What the focus pump and LoadMap do to the engine-owned primary stages
	// (FApp multiplier every frame; TransientPrimaryVolume on every travel):
	// neither is the master volume any more.
	FApp::SetVolumeMultiplier(1.0f);
	for (FAudioDevice* Device : Devices)
	{
		Device->SetTransientPrimaryVolume(1.0f);
	}
	TestEqual(TEXT("the master volume survives the focus pump and a map load"),
		FWoTGameSettingsAdapter::GetAppliedMasterVolume(), 0.35f);

	// Invalid settings never reach the devices.
	const int32 Before = Projected.Num();
	Settings.MasterVolume = 2.0f;
	FWoTGameSettingsAdapter::ApplyAudio(Settings);
	TestEqual(TEXT("an invalid master volume is refused"),
		FWoTGameSettingsAdapter::GetAppliedMasterVolume(), 0.35f);
	TestEqual(TEXT("an invalid master volume projects nothing"), Projected.Num(), Before);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
