#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "WoTTestBattleClasses.h"
#include "../Settings/WoTGameSettings.h"
#include "../WoTCharacterMovement.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameMode.h"
#include "../WoTRetailText.h"
#include "../WoTSoundTable.h"
#include "../WoTZoneVolume.h"

#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

#if WITH_EDITORONLY_DATA
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialInstance.h"
#endif

namespace
{
constexpr EAutomationTestFlags InputParityTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters InputParityAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

bool InputParityDriveAxis(UInputComponent& Input, FName AxisName, float Value)
{
	for (FInputAxisBinding& Binding : Input.AxisBindings)
	{
		if (Binding.AxisName == AxisName)
		{
			Binding.AxisDelegate.Execute(Value);
			return true;
		}
	}
	return false;
}

bool InputParityDriveAction(UInputComponent& Input, FName ActionName,
	EInputEvent Event, FKey Key)
{
	for (int32 Index = 0; Index < Input.GetNumActionBindings(); ++Index)
	{
		FInputActionBinding& Binding = Input.GetActionBinding(Index);
		if (Binding.GetActionName() == ActionName && Binding.KeyEvent == Event)
		{
			Binding.ActionDelegate.Execute(Key);
			return true;
		}
	}
	return false;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWalkingPredictionParitySpec,
	"WoT.Net.WalkingPredict", InputParityTestFlags)

bool FWoTWalkingPredictionParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	UWoTCharacterMovement* Move = Pawn
		? Cast<UWoTCharacterMovement>(Pawn->GetCharacterMovement()) : nullptr;
	if (!TestNotNull(TEXT("walking pawn"), Pawn)
		|| !TestNotNull(TEXT("walking movement component"), Move))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Move->SetMovementMode(MOVE_Walking);
	Move->SetWalkingRequested(true);
	Move->Velocity = FVector(400.0f, 0.0f, 0.0f);
	Move->SetAccelerationForTesting(FVector(2048.0f, 0.0f, 0.0f));
	constexpr float Step = 1.0f / 60.0f;
	constexpr float Friction = 8.0f;
	constexpr float AccelRate = 2048.0f;
	constexpr float MaxSpeed = 400.0f;
	Move->CalcVelocity(Step, Friction, false, 0.01f);
	TestTrue(TEXT("Walking clamps acceleration to 30 percent"),
		FMath::IsNearlyEqual(Move->GetAccelerationForTesting().Size(), 614.4f, 0.1f));
	const float AcceleratedSpeed = MaxSpeed + AccelRate * 0.3f * Step;
	const float ExpectedOneStep = FMath::Max(MaxSpeed * 0.3f,
		AcceleratedSpeed * (1.0f - Step * 2.0f * Friction));
	TestTrue(TEXT("Walking matches the exact retail accelerating step"),
		FMath::IsNearlyEqual(Move->Velocity.Size2D(), ExpectedOneStep, 0.01f));
	for (int32 Tick = 1; Tick < 120; ++Tick)
	{
		Move->SetAccelerationForTesting(FVector(AccelRate, 0.0f, 0.0f));
		Move->CalcVelocity(Step, Friction, false, 0.01f);
	}
	TestTrue(TEXT("Held walking input settles at 30 percent speed"),
		FMath::IsNearlyEqual(Move->Velocity.Size2D(), MaxSpeed * 0.3f, 0.01f));
	TestFalse(TEXT("Walking players do not step off ledges"), Move->CanWalkOffLedges());
	Move->Velocity.Z = 0.0f;
	Move->SetMovementMode(MOVE_Walking);
	TestTrue(TEXT("Walking players can jump"),
		Move->DoJump(/*bReplayingMoves*/ false, Step));

	FNetworkPredictionData_Client_Character* Prediction =
		Move->GetPredictionData_Client_Character();
	FSavedMovePtr SavedMove;
	if (Prediction) { SavedMove = Prediction->AllocateNewMove(); }
	if (TestTrue(TEXT("walking saved move allocated"), SavedMove.IsValid()))
	{
		SavedMove->SetMoveFor(Pawn, 1.0f / 60.0f, FVector::ZeroVector, *Prediction);
		const uint8 Flags = SavedMove->GetCompressedFlags();
		TestTrue(TEXT("walking uses a custom compressed flag"),
			(Flags & FSavedMove_Character::FLAG_Custom_0) != 0);

		AWoTCharacter* ReplayPawn = World->SpawnActor<AWoTCharacter>(
			FVector(500, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
		UWoTCharacterMovement* ReplayMove = ReplayPawn
			? Cast<UWoTCharacterMovement>(ReplayPawn->GetCharacterMovement()) : nullptr;
		if (TestNotNull(TEXT("replay movement component"), ReplayMove))
		{
			ReplayMove->UpdateFromCompressedFlags(Flags);
			TestTrue(TEXT("walking compressed flag round-trips"),
				ReplayMove->IsWalkingRequested());
		}
	}

	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(
		Pawn->GetActorLocation(), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	if (TestNotNull(TEXT("water zone"), Water))
	{
		Water->bWaterZone = true;
		Water->Collision->SetBoxExtent(FVector(1000.0f));
		TestFalse(TEXT("Walking request never slows swimming"), Move->IsRetailWalking());
		Move->Velocity = FVector(400.0f, 0.0f, 0.0f);
		Move->SetAccelerationForTesting(FVector(2048.0f, 0.0f, 0.0f));
		Move->CalcVelocity(1.0f / 60.0f, 8.0f, false, 0.01f);
		TestTrue(TEXT("water keeps full acceleration"),
			Move->GetAccelerationForTesting().Size() > 2000.0f);
	}

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayTauntParitySpec,
	"WoT.Input.PlayTaunt", InputParityTestFlags)

bool FWoTPlayTauntParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAesSedai* Pawn = World->SpawnActor<AWoTAesSedai>(
		FVector(0, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	AWoTTestMessageCaptureController* Controller =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	if (!TestNotNull(TEXT("taunt pawn"), Pawn)
		|| !TestNotNull(TEXT("taunt controller"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->Possess(Pawn);
	int32 SoundBurstSpawns = 0;
	const FDelegateHandle SoundBurstSpawnHandle = World->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateLambda(
			[&SoundBurstSpawns](AActor* SpawnedActor)
			{
				if (IsValid(SpawnedActor) && SpawnedActor->IsA<AWoTSoundBurst>())
				{
					++SoundBurstSpawns;
				}
			}));

	Controller->PlayTaunt(0);
	TestTrue(TEXT("invalid taunt reports the diagnostic"),
		Controller->CapturedMessages.Contains(
			TEXT("Taunt number is outside the available range.")));
	TestEqual(TEXT("invalid taunt does not play"), SoundBurstSpawns, 0);

	Pawn->bDead = true;
	Controller->PlayTaunt(1);
	TestEqual(TEXT("dead player taunt is a no-op"), SoundBurstSpawns, 0);
	Pawn->bDead = false;

	Controller->PlayTaunt(1);
	TestEqual(TEXT("valid taunt resolves the requested slot"),
		Controller->GetLastTauntSlotForTesting(), FName(TEXT("Taunt1")));
	const int32 AfterFirst = SoundBurstSpawns;
	TestEqual(TEXT("valid taunt emits one replicated sound"), AfterFirst, 1);
	Controller->PlayTaunt(2);
	TestEqual(TEXT("one-second spam gate suppresses the second taunt"),
		SoundBurstSpawns, AfterFirst);

	WoTTest::TickWorld(World, 61);
	Controller->PlayTaunt(-1);
	const FName RandomSlot = Controller->GetLastTauntSlotForTesting();
	TestTrue(TEXT("random taunt resolves to a valid slot"),
		RandomSlot.ToString().StartsWith(TEXT("Taunt"))
		&& FCString::Atoi(*RandomSlot.ToString().RightChop(5)) >= 1
		&& FCString::Atoi(*RandomSlot.ToString().RightChop(5)) <= 6);
	TestEqual(TEXT("random taunt emits after the gate"),
		SoundBurstSpawns, AfterFirst + 1);

	const UFunction* ServerFunction = AWoTPlayerController::StaticClass()
		->FindFunctionByName(TEXT("ServerPlayTaunt"));
	TestTrue(TEXT("PlayTaunt is a reliable server RPC"), ServerFunction
		&& ServerFunction->HasAnyFunctionFlags(FUNC_NetServer)
		&& ServerFunction->HasAnyFunctionFlags(FUNC_NetReliable));

	AWoTCharacter* SilentPawn = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	AWoTTestMessageCaptureController* SilentController =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	AWoTAesSedai* GatedPawn = World->SpawnActor<AWoTAesSedai>(
		FVector(1500, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	if (TestNotNull(TEXT("silent taunt pawn"), SilentPawn)
		&& TestNotNull(TEXT("silent taunt controller"), SilentController)
		&& TestNotNull(TEXT("post-silent gated pawn"), GatedPawn))
	{
		SilentController->Possess(SilentPawn);
		const int32 MessageCountBeforeSilentTaunt =
			SilentController->CapturedMessages.Num();
		const int32 SpawnsBeforeSilentTaunt = SoundBurstSpawns;
		SilentController->PlayTaunt(1);
		TestEqual(TEXT("missing taunt sound stays silent"),
			SilentController->CapturedMessages.Num(), MessageCountBeforeSilentTaunt);
		TestEqual(TEXT("missing taunt sound emits no burst"),
			SoundBurstSpawns, SpawnsBeforeSilentTaunt);
		SilentController->Possess(GatedPawn);
		SilentController->PlayTaunt(2);
		TestEqual(TEXT("missing taunt sound still arms the one-second gate"),
			SoundBurstSpawns, SpawnsBeforeSilentTaunt);
	}

	World->RemoveOnActorSpawnedHandler(SoundBurstSpawnHandle);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWalkingBothShiftsParitySpec,
	"WoT.Input.WalkingBothShifts", InputParityTestFlags)

bool FWoTWalkingBothShiftsParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* CharacterPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	AWoTTestMessageCaptureController* PlayerController =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	UWoTCharacterMovement* Movement = CharacterPawn
		? Cast<UWoTCharacterMovement>(CharacterPawn->GetCharacterMovement()) : nullptr;
	if (!TestNotNull(TEXT("walking chord pawn"), CharacterPawn)
		|| !TestNotNull(TEXT("walking chord controller"), PlayerController)
		|| !TestNotNull(TEXT("walking chord movement"), Movement))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerController->Possess(CharacterPawn);
	UInputComponent* Input = NewObject<UInputComponent>(CharacterPawn);
	CharacterPawn->SetupPlayerInputComponent(Input);

	TestTrue(TEXT("left Shift walking press reaches the runtime handler"),
		InputParityDriveAction(*Input, TEXT("Walking"), IE_Pressed, EKeys::LeftShift));
	TestTrue(TEXT("right Shift walking press reaches the runtime handler"),
		InputParityDriveAction(*Input, TEXT("Walking"), IE_Pressed, EKeys::RightShift));
	TestTrue(TEXT("either held Shift requests walking"), Movement->IsWalkingRequested());
	TestTrue(TEXT("left Shift release reaches the runtime handler"),
		InputParityDriveAction(*Input, TEXT("Walking"), IE_Released, EKeys::LeftShift));
	TestTrue(TEXT("releasing one Shift keeps walking while the other is held"),
		Movement->IsWalkingRequested());
	InputParityDriveAction(*Input, TEXT("Walking"), IE_Released, EKeys::RightShift);
	TestFalse(TEXT("releasing both Shift keys stops walking"),
		Movement->IsWalkingRequested());
	InputParityDriveAction(*Input, TEXT("Walking"), IE_Pressed, EKeys::LeftShift);
	PlayerController->UnPossess();
	TestFalse(TEXT("unpossess clears held walking state"),
		Movement->IsWalkingRequested());

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLookModeParitySpec,
	"WoT.Input.LookModes", InputParityTestFlags)

bool FWoTLookModeParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* CharacterPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	AWoTTestMessageCaptureController* PlayerController =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	if (!TestNotNull(TEXT("look-mode pawn"), CharacterPawn)
		|| !TestNotNull(TEXT("look-mode controller"), PlayerController))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Pitch input and the centre-view tick only act for a local player
	// controller (APawn::AddControllerPitchInput, IsLocallyControlled).
	PlayerController->SetAsLocalPlayerController();
	PlayerController->Possess(CharacterPawn);
	PlayerController->PlayerInput = NewObject<UPlayerInput>(PlayerController);
	FInputAxisProperties MouseYProperties;
	if (!PlayerController->PlayerInput->GetAxisProperties(
		EKeys::MouseY, MouseYProperties))
	{
		FInputAxisConfigEntry& Entry =
			PlayerController->PlayerInput->AxisConfig.AddDefaulted_GetRef();
		Entry.AxisKeyName = EKeys::MouseY.GetFName();
	}
	MouseYProperties.bInvert = true;
	PlayerController->PlayerInput->SetAxisProperties(EKeys::MouseY, MouseYProperties);
	UInputComponent* Input = NewObject<UInputComponent>(CharacterPawn);
	CharacterPawn->SetupPlayerInputComponent(Input);
	PlayerController->ClearRotationInputForTesting();
	CharacterPawn->ConsumeMovementInputVector();

	InputParityDriveAction(*Input, TEXT("Strafe"), IE_Pressed, EKeys::Apostrophe);
	TestTrue(TEXT("MouseX reaches the runtime handler"),
		InputParityDriveAxis(*Input, TEXT("MouseX"), 3.0f));
	TestTrue(TEXT("Turn reaches the runtime handler"),
		InputParityDriveAxis(*Input, TEXT("Turn"), 2.0f));
	const FVector StrafeInput = CharacterPawn->ConsumeMovementInputVector();
	TestTrue(TEXT("Strafe remaps turn keys and MouseX to lateral movement"),
		StrafeInput.Equals(FVector(0.0f, 5.0f, 0.0f), 0.001f));
	TestTrue(TEXT("Strafe suppresses yaw"),
		PlayerController->PendingRotationInputForTesting().IsNearlyZero());

	// PlayerInput has already inverted this positive physical MouseY movement.
	InputParityDriveAxis(*Input, TEXT("MouseY"), -4.0f);
	const FVector ForwardInput = CharacterPawn->ConsumeMovementInputVector();
	TestTrue(TEXT("inversion does not flip MouseY forward movement while strafing"),
		ForwardInput.Equals(FVector(4.0f, 0.0f, 0.0f), 0.001f));
	TestTrue(TEXT("Strafe suppresses MouseY pitch"),
		PlayerController->PendingRotationInputForTesting().IsNearlyZero());
	InputParityDriveAction(*Input, TEXT("Strafe"), IE_Released, EKeys::Apostrophe);

	// The controller's own pitch scale (legacy input scales are on in
	// DefaultInput.ini), measured rather than assumed.
	PlayerController->ClearRotationInputForTesting();
	PlayerController->AddPitchInput(1.0f);
	const float PitchScale = static_cast<float>(PlayerController->PendingRotationInputForTesting().Pitch);
	PlayerController->ClearRotationInputForTesting();
	InputParityDriveAxis(*Input, TEXT("MouseY"), -4.0f);
	TestEqual(TEXT("MouseY pitches when Strafe is not held"),
		static_cast<float>(PlayerController->PendingRotationInputForTesting().Pitch), -4.0f * PitchScale, 0.001f);
	TestTrue(TEXT("inverted MouseY does not add forward movement on the pitch branch"),
		CharacterPawn->GetPendingMovementInputVector().IsNearlyZero());

	PlayerController->ClearRotationInputForTesting();
	InputParityDriveAction(*Input, TEXT("FreeLook"), IE_Pressed, EKeys::Insert);
	InputParityDriveAxis(*Input, TEXT("MoveForward"), 6.0f);
	TestEqual(TEXT("FreeLook maps forward input to half-rate pitch"),
		static_cast<float>(PlayerController->PendingRotationInputForTesting().Pitch),
		3.0f * PitchScale, 0.01f); // FOVScale = 90 * 0.01111 ~= 1
	TestTrue(TEXT("FreeLook suppresses forward movement"),
		CharacterPawn->GetPendingMovementInputVector().IsNearlyZero());
	InputParityDriveAction(*Input, TEXT("FreeLook"), IE_Released, EKeys::Insert);

	PlayerController->ClearRotationInputForTesting();
	CastChecked<UCharacterMovementComponent>(CharacterPawn->GetCharacterMovement())
		->SetMovementMode(MOVE_Walking);
	PlayerController->SetControlRotation(FRotator(20.0f, 0.0f, 0.0f));
	InputParityDriveAction(*Input, TEXT("CenterView"), IE_Pressed, EKeys::End);
	CharacterPawn->Tick(1.0f / 120.0f);
	TestEqual(TEXT("same-frame explicit look suppresses centre-view decay"),
		static_cast<float>(PlayerController->GetControlRotation().Pitch), 20.0f, 0.001f);
	CharacterPawn->Tick(1.0f / 120.0f);
	TestEqual(TEXT("runtime centre-view decay uses 12 times the capped delta"),
		static_cast<float>(PlayerController->GetControlRotation().Pitch), 18.0f, 0.001f);
	TestTrue(TEXT("centre decay caps delta at 0.0833"), FMath::IsNearlyEqual(
		AWoTCharacter::DecayCenterViewPitch(20000.0f, 1.0f), 8.0f, 0.01f));
	TestEqual(TEXT("centre decay snaps below 1000 UE1 units"),
		AWoTCharacter::DecayCenterViewPitch(5.0f, 0.0f), 0.0f);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBrightnessParitySpec,
	"WoT.Input.Brightness", InputParityTestFlags)

bool FWoTBrightnessParitySpec::RunTest(const FString&)
{
	// Two full cycles from the retail default; every step must also pass the
	// settings validation the F11 save path runs.
	FWoTGameSettings Cycled = FWoTGameSettings::Defaults();
	TestEqual(TEXT("retail default brightness"), Cycled.Brightness, 0.6f, 0.0001f);
	const float Expected[] = {0.7f, 0.8f, 0.9f, 0.0f, 0.1f, 0.2f, 0.3f, 0.4f,
		0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 0.0f};
	for (const float Value : Expected)
	{
		Cycled.Brightness = FWoTGameSettingsAdapter::AdvanceBrightness(Cycled.Brightness);
		TestEqual(TEXT("brightness advances by a tenth and wraps"),
			Cycled.Brightness, Value, 0.0001f);
		FString ValidationError;
		const bool bValid = Cycled.Validate(ValidationError);
		TestTrue(FString::Printf(TEXT("cycled brightness %.2f passes validation: %s"),
			Cycled.Brightness, *ValidationError), bValid);
	}
	TestEqual(TEXT("brightness message uses the display index"),
		FWoTGameSettingsAdapter::BrightnessMessage(0.7f),
		FString(TEXT("Display brightness: 8 of 10")));
	const float BrightnessLevels[] = {0.0f, 0.1f, 0.2f, 0.6f, 0.9f};
	float PreviousExponent = MAX_flt;
	for (const float Level : BrightnessLevels)
	{
		const float RampInvGamma =
			FWoTGameSettingsAdapter::RampInvGammaForBrightness(Level);
		TestTrue(TEXT("brightness ramp exponent is strictly decreasing"),
			RampInvGamma < PreviousExponent);
		PreviousExponent = RampInvGamma;
	}
	// Retail Glide: grGammaCorrectionValue(0.5 + 1.5 * Brightness) (shipped
	// GlideDrv.dll).  The boundary material applies
	// displayed D^(1/g), so its exponent is the reciprocal of that ramp.
	TestTrue(TEXT("the 0.6 default reproduces retail Glide's 1.4 ramp"), FMath::IsNearlyEqual(
		FWoTGameSettingsAdapter::RampInvGammaForBrightness(0.6f), 1.0f / 1.4f, 1.0e-4f));
	TestTrue(TEXT("brightness level zero is Glide's 0.5 ramp (dark, not black)"),
		FMath::IsNearlyEqual(FWoTGameSettingsAdapter::RampInvGammaForBrightness(0.0f),
			1.0f / 0.5f, 1.0e-4f));
	TestTrue(TEXT("full brightness is Glide's 2.0 ramp"), FMath::IsNearlyEqual(
		FWoTGameSettingsAdapter::RampInvGammaForBrightness(1.0f), 1.0f / 2.0f, 1.0e-4f));
	TestTrue(TEXT("brightness below zero clamps to Glide's 0.5 ramp"), FMath::IsNearlyEqual(
		FWoTGameSettingsAdapter::RampInvGammaForBrightness(-1.0f), 1.0f / 0.5f, 1.0e-4f));
	TestTrue(TEXT("brightness above one clamps to Glide's 2.0 ramp"), FMath::IsNearlyEqual(
		FWoTGameSettingsAdapter::RampInvGammaForBrightness(2.0f), 1.0f / 2.0f, 1.0e-4f));

	UWorld* RampWorld = WoTTest::CreateSpecWorld();
	UMaterialParameterCollection* DisplayCollection =
		LoadObject<UMaterialParameterCollection>(nullptr,
			TEXT("/Game/WoT/Materials/MPC_WoTDisplay.MPC_WoTDisplay"));
	const float OriginalDisplayGamma = GEngine ? GEngine->DisplayGamma : 2.2f;
	FWoTGameSettingsAdapter::SetBrightnessWorldForTesting(RampWorld);
	ON_SCOPE_EXIT
	{
		FWoTGameSettingsAdapter::SetBrightnessWorldForTesting(nullptr);
		if (GEngine) { GEngine->DisplayGamma = OriginalDisplayGamma; }
		WoTTest::DestroySpecWorld(RampWorld);
	};
	if (TestNotNull(TEXT("generated display MPC"), DisplayCollection)
		&& TestNotNull(TEXT("brightness test world"), RampWorld))
	{
		if (GEngine) { GEngine->DisplayGamma = 2.2f; }
		const struct
		{
			float Brightness;
			float ExpectedExponent;
		} RampCases[] = {{0.6f, 1.0f / 1.4f}, {0.0f, 1.0f / 0.5f},
			{1.0f, 1.0f / 2.0f}};
		for (const auto& RampCase : RampCases)
		{
			FWoTGameSettingsAdapter::ApplyBrightness(RampCase.Brightness);
			TestTrue(TEXT("ApplyBrightness writes the exact display-ramp exponent"),
				FMath::IsNearlyEqual(UKismetMaterialLibrary::GetScalarParameterValue(
					RampWorld, DisplayCollection, TEXT("RampInvGamma")),
					RampCase.ExpectedExponent, 1.0e-4f));
			if (GEngine)
			{
				TestEqual(TEXT("ApplyBrightness leaves DisplayGamma at 2.2"),
					GEngine->DisplayGamma, 2.2f, 1.0e-4f);
			}
		}
	}

	FWoTGameSettings Saved = FWoTGameSettings::Defaults();
	Saved.Brightness = 0.8f;
	const FString Encoded = FWoTGameSettingsCodec::Serialize(Saved);
	const FTCHARToUTF8 Utf8(*Encoded);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	FWoTGameSettings Loaded;
	FString Error;
	TestTrue(TEXT("brightness settings parse"),
		FWoTGameSettingsCodec::Parse(Bytes, Loaded, Error));
	TestEqual(TEXT("brightness persists"), Loaded.Brightness, 0.8f);
	return !HasAnyErrors();
}

#if WITH_EDITORONLY_DATA
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGammaBoundaryMaterialSpec,
	"WoT.Input.GammaBoundaryMaterial", InputParityTestFlags)

bool FWoTGammaBoundaryMaterialSpec::RunTest(const FString&)
{
	UMaterial* Boundary = LoadObject<UMaterial>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_GammaBoundary.M_WoT_GammaBoundary"));
	UMaterialInstance* Instance = LoadObject<UMaterialInstance>(nullptr,
		TEXT("/Game/WoT/Materials/Instances/MI_GammaBoundary.MI_GammaBoundary"));
	UMaterialParameterCollection* Collection =
		LoadObject<UMaterialParameterCollection>(nullptr,
			TEXT("/Game/WoT/Materials/MPC_WoTDisplay.MPC_WoTDisplay"));
	if (!TestNotNull(TEXT("generated gamma-boundary material"), Boundary)
		|| !TestNotNull(TEXT("generated gamma-boundary instance"), Instance)
		|| !TestNotNull(TEXT("generated display-ramp collection"), Collection))
	{
		return false;
	}
	TestTrue(TEXT("the boundary instance still parents the generated material"),
		Instance->Parent == Boundary);

	const UMaterialExpressionCustom* Decode = nullptr;
	const UMaterialExpressionCollectionParameter* Ramp = nullptr;
	for (const UMaterialExpression* Expression : Boundary->GetExpressions())
	{
		if (const UMaterialExpressionCustom* Custom =
			Cast<UMaterialExpressionCustom>(Expression))
		{
			if (Custom->Code.Contains(TEXT("pow(saturate(In.rgb), K)"))
				&& Custom->Code.Contains(TEXT("D / 12.92"))
				&& Custom->Code.Contains(TEXT("step(0.04045, D)")))
			{
				Decode = Custom;
			}
		}
		if (const UMaterialExpressionCollectionParameter* Parameter =
			Cast<UMaterialExpressionCollectionParameter>(Expression))
		{
			if (Parameter->Collection == Collection
				&& Parameter->ParameterName == TEXT("RampInvGamma"))
			{
				Ramp = Parameter;
			}
		}
	}
	TestNotNull(TEXT("the custom boundary applies the Glide ramp before sRGB decode"),
		Decode);
	TestNotNull(TEXT("the boundary reads MPC_WoTDisplay.RampInvGamma"), Ramp);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPauseParitySpec,
	"WoT.Input.Pause", InputParityTestFlags)

bool FWoTPauseParitySpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping loaded pause refusal"));
		return true;
	}
	const FString* NoPauseMessage = FWoTRetailText::FindSectionValue(
		TEXT("PlayerPawn"), TEXT("NoPauseMessage"));
	if (!TestNotNull(TEXT("NoPauseMessage is loaded"), NoPauseMessage))
	{
		return false;
	}
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestMessageCaptureController* Controller =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	AWoTGameMode* GameMode = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("pause controller"), Controller)
		|| !TestNotNull(TEXT("pause game mode"), GameMode))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Controller->HandlePauseRequestForTesting(NM_ListenServer);
	TestTrue(TEXT("multiplayer pause is refused with the retail message"),
		Controller->CapturedMessages.Contains(*NoPauseMessage));
	TestFalse(TEXT("multiplayer game remains unpaused"),
		World->IsPaused());

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHeadTossSavedMoveCombineSpec,
	"WoT.Net.HeadTossSavedMoveCombine", InputParityTestFlags)

// A live creature presentation pitch (BATrolloc HeadToss) makes the client's
// saved move non-combinable so replay cannot restore the cleared event pitch;
// once the pitch clears, moves combine again (no lasting bandwidth cost).
bool FWoTHeadTossSavedMoveCombineSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, InputParityAlwaysSpawn());
	UWoTCharacterMovement* Move = Pawn
		? Cast<UWoTCharacterMovement>(Pawn->GetCharacterMovement()) : nullptr;
	FNetworkPredictionData_Client_Character* Prediction =
		Move ? Move->GetPredictionData_Client_Character() : nullptr;
	if (!TestNotNull(TEXT("head-toss pawn"), Pawn)
		|| !TestNotNull(TEXT("head-toss movement component"), Move)
		|| !TestNotNull(TEXT("client prediction data"), Prediction))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Move->SetMovementMode(MOVE_Walking);
	constexpr float Step = 1.0f / 60.0f;

	// Baseline: whatever UE decides for two identical moves before any pitch.
	bool bBaselineCombines = false;
	{
		FSavedMovePtr BaseOld = Prediction->AllocateNewMove();
		FSavedMovePtr BaseNew = Prediction->AllocateNewMove();
		if (TestTrue(TEXT("baseline saved moves allocated"),
				BaseOld.IsValid() && BaseNew.IsValid()))
		{
			BaseOld->SetMoveFor(Pawn, Step, FVector::ZeroVector, *Prediction);
			BaseNew->SetMoveFor(Pawn, Step, FVector::ZeroVector, *Prediction);
			TestFalse(TEXT("baseline moves are not forced apart"),
				BaseOld->bForceNoCombine || BaseNew->bForceNoCombine);
			bBaselineCombines = BaseNew->CanCombineWith(
				BaseOld, Pawn, Prediction->MaxMoveDeltaTime);
		}
	}

	Pawn->SetCreaturePresentationPitch(22.5f);
	TestTrue(TEXT("head-toss pitch is live"),
		Pawn->IsCreaturePresentationPitchActive());
	FSavedMovePtr PitchMove = Prediction->AllocateNewMove();
	if (TestTrue(TEXT("pitch saved move allocated"), PitchMove.IsValid()))
	{
		PitchMove->SetMoveFor(Pawn, Step, FVector::ZeroVector, *Prediction);
		TestTrue(TEXT("a move made while the pitch is live never combines"),
			PitchMove->bForceNoCombine);
	}

	// One retained presentation update, then the ordinary update clears it.
	for (int32 Update = 0;
		Update < 4 && Pawn->IsCreaturePresentationPitchActive(); ++Update)
	{
		Pawn->ClearCreaturePresentationPitch();
	}
	TestFalse(TEXT("head-toss pitch has cleared"),
		Pawn->IsCreaturePresentationPitchActive());

	FSavedMovePtr OldMove = Prediction->AllocateNewMove();
	FSavedMovePtr NewMove = Prediction->AllocateNewMove();
	if (TestTrue(TEXT("post-clear saved moves allocated"),
			OldMove.IsValid() && NewMove.IsValid()))
	{
		OldMove->SetMoveFor(Pawn, Step, FVector::ZeroVector, *Prediction);
		NewMove->SetMoveFor(Pawn, Step, FVector::ZeroVector, *Prediction);
		TestFalse(TEXT("a move made after the pitch clears is combinable"),
			NewMove->bForceNoCombine || OldMove->bForceNoCombine);
		TestEqual(TEXT("post-clear combining matches the never-pitched baseline"),
			NewMove->CanCombineWith(OldMove, Pawn, Prediction->MaxMoveDeltaTime),
			bBaselineCombines);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
