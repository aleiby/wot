#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTZoneLeeches.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTPlayerController.h"
#include "../WoTSoundTable.h"
#include "../WoTZoneVolume.h"

#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
constexpr EAutomationTestFlags ParityBFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters ParityBSpawn()
{
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return P;
}

AWoTZoneVolume* ParityBZone(UWorld& World, const FVector& Center,
	const FVector& Extent, bool bWater = false)
{
	AWoTZoneVolume* Zone = World.SpawnActor<AWoTZoneVolume>(
		Center, FRotator::ZeroRotator, ParityBSpawn());
	Zone->Collision->SetBoxExtent(Extent);
	Zone->bWaterZone = bWater;
	return Zone;
}

AActor* ParityBFloor(UWorld& World)
{
	AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(
		FVector(0,0,-50), FRotator::ZeroRotator, ParityBSpawn());
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("ParityBFloor"));
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->SetBoxExtent(FVector(2000,2000,50));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	return Floor;
}

struct FParityBPlayer
{
	AWoTPlayerController* PC = nullptr;
	AWoTCharacter* Pawn = nullptr;
};

FParityBPlayer ParityBPlayer(UWorld& World, const FVector& Location)
{
	FParityBPlayer P;
	P.PC = World.SpawnActor<AWoTPlayerController>(ParityBSpawn());
	P.Pawn = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, ParityBSpawn());
	P.PC->SetAsLocalPlayerController();
	P.PC->Possess(P.Pawn);
	return P;
}

bool ParityBLoadArena12(const FString& Path, UWoTMapLightTable& Table)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)) return false;
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return false;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Root->TryGetArrayField(TEXT("zone_bsp"), Nodes)) return false;
	for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
	{
		const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Plane = nullptr;
		if (!Node.IsValid() || !Node->TryGetArrayField(TEXT("plane"), Plane)
			|| Plane->Num() != 4) return false;
		Table.ZoneBspPlanes.Add(FVector4(
			(*Plane)[0]->AsNumber(), (*Plane)[1]->AsNumber(),
			(*Plane)[2]->AsNumber(), (*Plane)[3]->AsNumber()));
		Table.ZoneBspFront.Add(Node->GetIntegerField(TEXT("front")));
		Table.ZoneBspBack.Add(Node->GetIntegerField(TEXT("back")));
	}
	return !Table.ZoneBspPlanes.IsEmpty();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityBExactZoneMembershipSpec,
	"WoT.Parity.B.ExactZoneMembership", ParityBFlags)
bool FParityBExactZoneMembershipSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Synthetic = NewObject<UWoTMapLightTable>(World);
	Synthetic->ZoneBspPlanes = {FVector4(1,0,0,0)};
	Synthetic->ZoneBspFront = {-3}; // zone 2
	Synthetic->ZoneBspBack = {-2};  // zone 1
	UWoTMeshLight::RegisterRuntimeMapLightTable(World, Synthetic);
	AWoTZoneVolume* Zone = ParityBZone(*World, FVector::ZeroVector, FVector(100));
	Zone->ProjectedZoneIndex = 1;
	TestTrue(TEXT("synthetic BSP accepts the volume's own leaf"),
		Zone->ContainsPoint(FVector(-10,0,0)));
	TestFalse(TEXT("synthetic BSP rejects a neighbour inside the same AABB"),
		Zone->ContainsPoint(FVector(10,0,0)));

	constexpr FStringView ArenaRelative = TEXT("exports/zones/Arena_12.zones.json");
	const FString ArenaPath = WoTReferencePath(ArenaRelative);
	if (!FPaths::FileExists(ArenaPath))
	{
		AddWarning(WoTReferenceMissingWarning(ArenaRelative));
	}
	else
	{
		UWoTMapLightTable* Arena12 = NewObject<UWoTMapLightTable>(World);
		if (TestTrue(TEXT("Arena_12 exported zone BSP fixture loads"),
			ParityBLoadArena12(ArenaPath, *Arena12)))
		{
			UWoTMeshLight::RegisterRuntimeMapLightTable(World, Arena12);
			AWoTZoneVolume* Basement = ParityBZone(*World,
				FVector(2594.0004f,-320.0f,-539.0f),
				FVector(898.0004f,1728.0f,165.0f), true);
			Basement->ProjectedZoneIndex = 6;
			const FVector Counterexample(2000,-1500,-600);
			TestTrue(TEXT("Arena_12 counterexample is inside WaterZone2's projected box"),
				Basement->Collision->Bounds.GetBox().IsInsideOrOn(Counterexample));
			TestFalse(TEXT("Arena_12 neighbour leaf inside the box is not water"),
				Basement->ContainsPoint(Counterexample));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityBLightningFootZoneSpec,
	"WoT.Parity.B.LightningFootZone", ParityBFlags)
bool FParityBLightningFootZoneSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ParityBFloor(*World);
	AWoTZoneVolume* Water = ParityBZone(*World,
		FVector(200,0,100), FVector(200,500,200), true);
	FParityBPlayer Player = ParityBPlayer(*World, FVector(-100,0,46));
	AWoTAngrealInvLightning* Lightning =
		World->SpawnActor<AWoTAngrealInvLightning>(ParityBSpawn());
	Lightning->CurCharges = 20;
	Lightning->GiveTo(Player.Pawn);
	Player.Pawn->InventoryComponent->SelectItem(Lightning);
	// DefaultCastReflector's retail one-cast-per-tick gate is strict `>`.
	WoTTest::TickWorld(World, 0.05f);
	Player.Pawn->InventoryComponent->UseAngreal();
	TestTrue(TEXT("production inventory entry starts the lightning cast"),
		Lightning->bCasting);
	const int32 DryHealth = Player.Pawn->Health;
	bool bEntered = false;
	bool bSameFrameShock = false;
	for (int32 Frame = 0; Frame < 90 && !bEntered; ++Frame)
	{
		Player.Pawn->AddMovementInput(FVector::ForwardVector, 1.0f);
		const int32 Before = Player.Pawn->Health;
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		UWoTZoneIndex* Index = World->GetSubsystem<UWoTZoneIndex>();
		bEntered = Index->GetFootRegion(Player.Pawn) == Water;
		if (bEntered) bSameFrameShock = Player.Pawn->Health < Before;
	}
	TestTrue(TEXT("the walking pawn reaches the water foot region"), bEntered);
	TestTrue(TEXT("FootZoneChange shocks the lightning caster in the entry frame"),
		bSameFrameShock && Player.Pawn->Health < DryHealth);
	TestFalse(TEXT("an unshielded wet caster immediately drops lightning"),
		Lightning->bCasting);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityBFOVLeechZoneSpec,
	"WoT.Parity.B.FOVAndLeechZone", ParityBFlags)
bool FParityBFOVLeechZoneSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Zone = ParityBZone(*World,
		FVector(300,0,100), FVector(100,300,300));
	Zone->bFOVZone = true; Zone->ZoneFOV = 120.0f; Zone->bLeechZone = true;
	Zone->LeechClasses.Add(UWoTSmoothRandFOVLeech::StaticClass());
	FParityBPlayer Player = ParityBPlayer(*World, FVector::ZeroVector);
	if (Player.Pawn->SetActorLocation(FVector(300,0,100)))
	{
		UWoTZoneIndex::RefreshAfterRelocation(Player.Pawn);
	}
	TestEqual(TEXT("ActorEntered applies the authored ZoneFOV"),
		Player.PC->GetCameraFOVForTest(), 120.0f);
	TestNotNull(TEXT("ActorEntered installs the authored FOV leech"),
		Player.Pawn->FindComponentByClass<UWoTSmoothRandFOVLeech>());
	if (Player.Pawn->SetActorLocation(FVector::ZeroVector))
	{
		UWoTZoneIndex::RefreshAfterRelocation(Player.Pawn);
	}
	TestEqual(TEXT("ActorLeaving restores the player's default FOV"),
		Player.PC->GetCameraFOVForTest(), 90.0f);
	TestNull(TEXT("ActorLeaving removes the zone-tagged leech"),
		Player.Pawn->FindComponentByClass<UWoTSmoothRandFOVLeech>());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityBCarcassWaterZoneSpec,
	"WoT.Parity.B.CarcassWaterZone", ParityBFlags)
bool FParityBCarcassWaterZoneSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = ParityBZone(*World,
		FVector(0,0,0), FVector(300,300,100), true);
	Water->EntrySoundSubPath = TEXT("AmbientA/DIVE");
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector(0,0,220), FRotator::ZeroRotator, ParityBSpawn());
	Carcass->Mass = 100.0f; Carcass->Buoyancy = 150.0f;
	Carcass->SetVelocityForTest(FVector(0,0,-300));
	for (int32 Frame = 0; Frame < 45 && !Carcass->HasSplashedForTest(); ++Frame)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	}
	TestTrue(TEXT("falling carcass dispatches ZoneChange on water entry"),
		Carcass->HasSplashedForTest());
	TestTrue(TEXT("buoyant carcass flattens its water collision"),
		Carcass->WasWaterFlattenedForTest());
	bool bSplashMarker = false;
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		if (It->SoundSubPath == Water->EntrySoundSubPath) bSplashMarker = true;
	}
	TestTrue(TEXT("fast dry-to-water entry emits the zone splash sound"), bSplashMarker);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
