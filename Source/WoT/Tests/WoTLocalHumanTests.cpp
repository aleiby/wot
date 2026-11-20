#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "WoTTestBotCasting.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../WoTCharacter.h"
#include "../WoTOnboardingSeams.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "../Testing/WoTNetTestSnapshot.h"
#include "AIController.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/ScopeExit.h"
#include "WoTLaunchEngineHooks.h"

namespace WoTLocalHumanSpec
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

struct FWorld
{
	UWorld* World = WoTTest::CreateSpecWorld();
	~FWorld() { WoTTest::DestroySpecWorld(World); }

	AWoTCharacter* SpawnBody(bool bHuman)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Body = World->SpawnActor<AWoTCharacter>(
			FVector(bHuman ? 1000.0f : 0.0f, 0, 200), FRotator::ZeroRotator, Params);
		if (bHuman)
		{
			AWoTPlayerController* Driver = World->SpawnActor<AWoTPlayerController>(Params);
			Driver->SetAsLocalPlayerController();
			Driver->Possess(Body);
		}
		else
		{
			World->SpawnActor<AAIController>(Params)->Possess(Body);
		}
		Body->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		return Body;
	}

	AWoTTestBotCastingItem* Item(AWoTCharacter* Body)
	{
		const FTransform Transform(Body->GetActorTransform());
		AWoTTestBotCastingItem* Result = World->SpawnActorDeferred<AWoTTestBotCastingItem>(
			AWoTTestBotCastingItem::StaticClass(), Transform, Body, Body,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Result->bHeldItem = true;
		Result->CurCharges = 20;
		Result->FinishSpawning(Transform);
		return Result;
	}
};

UWoTHandSet* EmptyHands(AWoTCharacter* Body, AWoTInventory* Item)
{
	UWoTHandSet* Hands = NewObject<UWoTHandSet>(Body);
	Hands->Init(Body);
	Hands->GetHand(1)->AddClassName(UWoTInventoryComponent::GetHandSetItemName(Item), 0);
	Body->AngrealHandSet = Hands;
	Body->CurrentHandSet = Hands;
	return Hands;
}

void InventoryRep(UWoTInventoryComponent* Inventory)
{
	Inventory->ProcessEvent(Inventory->FindFunctionChecked(TEXT("OnRep_Inventory")), nullptr);
}

void SelectionRep(UWoTInventoryComponent* Inventory)
{
	struct FParameters { AWoTInventory* PreviousSelectedItem = nullptr; } Parameters;
	Inventory->ProcessEvent(Inventory->FindFunctionChecked(TEXT("OnRep_SelectedItem")), &Parameters);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanPickupSpec,
	"WoT.LocalHuman.OnboardingPickup", WoTLocalHumanSpec::Flags)
bool FWoTLocalHumanPickupSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanSpec;
	FWorld Fixture;
	AWoTCharacter* Bot = Fixture.SpawnBody(false);
	AWoTCharacter* Human = Fixture.SpawnBody(true);
	TestTrue(TEXT("AI reproduces the engine local-control condition"), Bot->IsLocallyControlled());
	TArray<FName> Announced;
	auto& Seam = FWoTOnboardingSeams::Get().ClientItemAdded;
	const FDelegateHandle Handle = Seam.AddLambda([&Announced](FName Name) { Announced.Add(Name); });
	Bot->InventoryComponent->AddInventory(Fixture.Item(Bot));
	InventoryRep(Bot->InventoryComponent);
	TestEqual(TEXT("bot pickup and replication never announce an item"), Announced.Num(), 0);
	Announced.Reset();
	AWoTInventory* First = Fixture.Item(Human);
	Human->InventoryComponent->AddInventory(First);
	Human->InventoryComponent->AddInventory(Fixture.Item(Human));
	InventoryRep(Human->InventoryComponent);
	TestEqual(TEXT("human pickup announces a class exactly once"), Announced.Num(), 1);
	if (Announced.Num() == 1)
		TestEqual(TEXT("the acquired class is announced"), Announced[0], UWoTInventoryComponent::GetHandSetItemName(First));
	AWoTInventory* OtherClass = Fixture.World->SpawnActor<AWoTSeal>(
		FVector(3000, 0, 200), FRotator::ZeroRotator);
	Human->InventoryComponent->AddInventory(OtherClass);
	InventoryRep(Human->InventoryComponent);
	TestEqual(TEXT("a second human item class announces exactly once"), Announced.Num(), 2);
	if (Announced.Num() == 2)
		TestEqual(TEXT("the second class is announced"), Announced[1], UWoTInventoryComponent::GetHandSetItemName(OtherClass));
	Seam.Remove(Handle);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FWoTLocalHumanInventoryHandsSpec,
	"WoT.LocalHuman.InventoryHands", WoTLocalHumanSpec::Flags)
void FWoTLocalHumanInventoryHandsSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : {TEXT("Pickup"), TEXT("Selection"), TEXT("InventoryReplication"),
		TEXT("SelectionReplication"), TEXT("DeleteSelected"), TEXT("DeleteUnselected")})
	{
		Names.Add(Name);
		Commands.Add(Name);
	}
}
bool FWoTLocalHumanInventoryHandsSpec::RunTest(const FString& Operation)
{
	using namespace WoTLocalHumanSpec;
	FWorld Fixture;
	for (bool bHuman : {false, true})
	{
		AWoTCharacter* Body = Fixture.SpawnBody(bHuman);
		AWoTInventory* Item = Fixture.Item(Body);
		UWoTInventoryComponent* Inventory = Body->InventoryComponent;
		UWoTHandSet* Hands = EmptyHands(Body, Item);
		const bool bDelete = Operation.StartsWith(TEXT("Delete"));
		if (Operation != TEXT("Pickup")) Inventory->Items.Add(Item);
		if (bDelete)
		{
			Hands->AddItem(UWoTInventoryComponent::GetHandSetItemName(Item), Inventory);
			Inventory->SelectedItem = Operation == TEXT("DeleteSelected") ? Item : nullptr;
			TestTrue(TEXT("deletion still updates gameplay inventory"), Inventory->DeleteInventory(Item));
		}
		else if (Operation == TEXT("Pickup")) Inventory->AddInventory(Item);
		else if (Operation == TEXT("Selection")) Inventory->SelectItem(Item);
		else if (Operation == TEXT("InventoryReplication")) InventoryRep(Inventory);
		else
		{
			Inventory->SelectedItem = Item;
			SelectionRep(Inventory);
		}
		TestEqual(bHuman ? TEXT("human hand receives the notification") : TEXT("bot hand receives no notification"),
			Hands->GetHand(1)->bHaveThis[0], bDelete ? !bHuman : bHuman);
		if (!bDelete)
			TestTrue(TEXT("the body retains its gameplay item"), Inventory->Items.Contains(Item));
	}
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FWoTLocalHumanHandsEntrySpec,
	"WoT.LocalHuman.HandsEntry", WoTLocalHumanSpec::Flags)
void FWoTLocalHumanHandsEntrySpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : {TEXT("Ensure"), TEXT("Reconcile"), TEXT("EditorEnsure"),
		TEXT("EditorReconcile"), TEXT("EditorState")})
	{
		Names.Add(Name);
		Commands.Add(Name);
	}
}
bool FWoTLocalHumanHandsEntrySpec::RunTest(const FString& Operation)
{
	using namespace WoTLocalHumanSpec;
	FWorld Fixture;
	for (bool bHuman : {false, true})
	{
		AWoTCharacter* Body = Fixture.SpawnBody(bHuman);
		Body->AngrealHandSet = nullptr;
		Body->CitadelEditorHandSet = nullptr;
		Body->CurrentHandSet = nullptr;
		if (Operation == TEXT("Ensure")) Body->EnsureAngrealHands();
		else if (Operation == TEXT("Reconcile")) Body->ReconcileHandSet();
		else if (Operation == TEXT("EditorEnsure")) Body->EnsureCitadelEditorHands();
		else if (Operation == TEXT("EditorReconcile")) Body->ReconcileCitadelEditorHandSet();
		else Body->HandleCitadelEditStateChanged(true);
		const bool bCreated = Operation.StartsWith(TEXT("Editor"))
			? Body->CitadelEditorHandSet != nullptr : Body->AngrealHandSet != nullptr;
		TestEqual(TEXT("only a human creates a local hand set"), bCreated, bHuman);
		if (Operation == TEXT("EditorState"))
			TestEqual(TEXT("only a human enters editor input"), Body->IsCitadelEditorInputActive(), bHuman);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanBotCastingSpec,
	"WoT.LocalHuman.ServerFireAndCast", WoTLocalHumanSpec::Flags)
bool FWoTLocalHumanBotCastingSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanSpec;
	FWorld Fixture;
	AWoTCharacter* Bot = Fixture.SpawnBody(false);
	AWoTTestBotCastingItem* Item = Fixture.Item(Bot);
	Bot->InventoryComponent->AddInventory(Item);
	WoTTest::TickWorld(Fixture.World, 0.2f);
	const FRotator Aim(12, 35, 0);
	TestTrue(TEXT("server fire activates the bot item"), Bot->AuthorityUseAngreal(Aim));
	TestEqual(TEXT("server fire reaches the owned item"), Item->ActivationCount, 1);
	TestTrue(TEXT("server fire owns the animation latch"), Bot->IsPlayerAnimFireHeldForTesting());
	TestTrue(TEXT("server fire keeps chosen aim"), Bot->GetControlRotation().Equals(Aim));
	Bot->AuthorityCeaseUsingAngreal();
	TestFalse(TEXT("server release clears the latch"), Bot->IsPlayerAnimFireHeldForTesting());
	WoTTest::TickWorld(Fixture.World, 1.0f);
	UWoTCasterComponent* Caster = NewObject<UWoTCasterComponent>(Bot);
	Bot->AddInstanceComponent(Caster);
	Caster->RegisterComponent();
	Caster->BeginCast(Item);
	TestTrue(TEXT("bot caster still activates"), Caster->IsCasting());
	TestEqual(TEXT("bot caster reaches the same item"), Item->ActivationCount, 2);
	TestTrue(TEXT("bot casting keeps the animation latch"), Bot->IsPlayerAnimFireHeldForTesting());
	Caster->Release();
	TestNull(TEXT("server firing and casting create no human hand set"), Bot->AngrealHandSet.Get());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanLookSpec,
	"WoT.LocalHuman.LookPreferences", WoTLocalHumanSpec::Flags)
bool FWoTLocalHumanLookSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanSpec;
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	ON_SCOPE_EXIT { UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false); };
	FWoTEarlyUIAuthority& EarlyUIAuthority = GetEarlyUIAuthority();
	EarlyUIAuthority.ResetForTesting();
	ON_SCOPE_EXIT { EarlyUIAuthority.ResetForTesting(); };
	const ANSICHAR* Text = "Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n";
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	auto EarlyUI = ParseEarlyUIV1(Bytes, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
	if (!TestFalse(TEXT("look fixture preferences parse"), EarlyUI.IsError())) return false;
	if (!TestFalse(TEXT("look fixture preferences publish"),
		EarlyUIAuthority.PublishOnce(MoveTemp(EarlyUI).GetValue()).IsError())) return false;
	const FWoTEarlyUINativeProjectionSink ProjectionSink{
		[](uint16, uint16, EWoTWindowMode) { return FWoTBootstrapStatus::Success(); }};
	FString Error;
	if (!TestTrue(TEXT("look fixture projection applies"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(ProjectionSink, &Error))) return false;
	FWorld Fixture;
	UWoTPreferencesSubsystem* Preferences = Fixture.World->GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>();
	if (!TestNotNull(TEXT("look preferences"), Preferences)) return false;
	FWoTGameSettings Settings = Preferences->GetGameSettings();
	Settings.bAlwaysMouseLook = false;
	Settings.bLookSpring = true;
	Settings.bAutoSlopeLook = false;
	Preferences->SetGameSettingsForTesting(Settings);
	for (bool bHuman : {false, true})
	{
		AWoTCharacter* Body = Fixture.SpawnBody(bHuman);
		Body->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		Body->GetController()->SetControlRotation(FRotator(30, 0, 0));
		Body->Tick(0.1f);
		const float Expected = bHuman ? AWoTCharacter::DecayCenterViewPitch(30.0f, 0.1f) : 30.0f;
		TestEqual(bHuman ? TEXT("human look spring still centers the view") : TEXT("host look spring does not alter bot aim"),
			static_cast<float>(Body->GetControlRotation().Pitch), Expected);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanPitchCleanupSpec,
	"WoT.LocalHuman.PitchCleanup", WoTLocalHumanSpec::Flags)
bool FWoTLocalHumanPitchCleanupSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanSpec;
	FWorld Fixture;
	AWoTCharacter* Bot = Fixture.SpawnBody(false);
	AWoTCharacter* Human = Fixture.SpawnBody(true);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTNPCCharacter* NPC = Fixture.World->SpawnActor<AWoTNPCCharacter>(
		FVector(2000, 0, 200), FRotator::ZeroRotator, Params);
	Fixture.World->SpawnActor<AAIController>(Params)->Possess(NPC);
	for (AWoTCharacter* Body : {Bot, Human, static_cast<AWoTCharacter*>(NPC)})
	{
		Body->MulticastCreaturePresentationPitch_Implementation(15.0f);
		Body->Tick(0.01f);
		Body->Tick(0.01f);
	}
	TestFalse(TEXT("bot tick expires the presentation pulse without a human view update"), Bot->IsCreaturePresentationPitchActive());
	TestTrue(TEXT("human tick leaves cleanup to its view update"), Human->IsCreaturePresentationPitchActive());
	TestTrue(TEXT("NPC retains its existing presentation lifecycle"), NPC->IsCreaturePresentationPitchActive());
	Human->ClearCreaturePitchAfterNormalViewUpdate();
	Human->ClearCreaturePitchAfterNormalViewUpdate();
	TestFalse(TEXT("human view updates still expire the pulse"), Human->IsCreaturePresentationPitchActive());
	return !HasAnyErrors();
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalHumanSnapshotSpec,
	"WoT.LocalHuman.Snapshot", WoTLocalHumanSpec::Flags)
bool FWoTLocalHumanSnapshotSpec::RunTest(const FString&)
{
	using namespace WoTLocalHumanSpec;
	FWorld Fixture;
	AWoTCharacter* Bot = Fixture.SpawnBody(false);
	AWoTCharacter* Human = Fixture.SpawnBody(true);
	const auto Snapshot = WoTNetTest::BuildSnapshot(Fixture.World);
	int32 Checked = 0;
	for (const auto& Value : Snapshot->GetArrayField(TEXT("pawns")))
	{
		const auto& Pawn = Value->AsObject();
		const FString Name = Pawn->GetStringField(TEXT("name"));
		if (Name != Bot->GetName() && Name != Human->GetName()) continue;
		const bool bHuman = Name == Human->GetName();
		TestEqual(TEXT("snapshot identifies only the local human"), Pawn->GetBoolField(TEXT("local")), bHuman);
		TestEqual(TEXT("only the local human exposes hand diagnostics"), Pawn->HasField(TEXT("hand_classes")), bHuman);
		++Checked;
	}
	TestEqual(TEXT("both bodies were inspected"), Checked, 2);
	return !HasAnyErrors();
}
#endif

#endif
