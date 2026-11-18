#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTDecoration.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
constexpr EAutomationTestFlags WoTDecorationPhysicsSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

constexpr uint8 WoTPhysicsNone = 0;
constexpr uint8 WoTPhysicsFalling = 2;
constexpr uint8 WoTPhysicsRotating = 5;

class FWoTDecorationPhysicsObjectWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit,
				TEXT("decoration-physics retail writer overflow")));
		}
		if (Schema == TEXT("wot-world-v3")
			|| Schema == TEXT("wot-class-default-v3"))
		{
			TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
			Copy.Append(Bytes.GetData(), Bytes.Num());
		}
		return FWoTBootstrapStatus::Success();
	}

	TMap<FString, TArray<uint8>> Objects;
};

class FWoTDecorationPhysicsProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

FString WoTDecorationPhysicsLeaf(FStringView Identity)
{
	FString Result(Identity);
	int32 Dot = INDEX_NONE;
	if (Result.FindLastChar(TEXT('.'), Dot)) Result.RightChopInline(Dot + 1);
	return Result;
}

struct FWoTDecorationPhysicsRetailFixture
{
	bool Load(FAutomationTestBase& Test)
	{
		if (bAttempted)
		{
			if (!bLoaded) Test.AddError(Error);
			return bLoaded;
		}
		bAttempted = true;
		if (!WoTReferenceTreeExists())
		{
			Error = TEXT("real retail data absent: decoration physics not verified");
			Test.AddError(Error);
			return false;
		}

		const FString RetailRoot = WoTReferencePath(TEXT("original"));
		TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
			WoTLoadProductionRetailContract();
		if (LoadedContract.IsError())
		{
			Error = LoadedContract.GetError().ToString();
			Test.AddError(Error);
			return false;
		}

		FWoTRetailContract WorldContract;
		FWoTRetailContract DefaultsContract;
		TArray<FWoTWorldPackageBytesForTesting> WorldSources;
		TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
		TArray<FString> DefaultLogicalIds;
		for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
		{
			const bool bMultiplayerWorld =
				(Fact.Domain == EWoTFactDomain::World
					|| Fact.Domain == EWoTFactDomain::BSP)
				&& !Fact.SourcePath.Equals(
					TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
			if (bMultiplayerWorld) WorldContract.Facts.Add(Fact);
			if (bMultiplayerWorld && Fact.Domain == EWoTFactDomain::World)
			{
				FWoTWorldPackageBytesForTesting& Source =
					WorldSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(
					Source.Bytes, *(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
			}
			if (Fact.Domain == EWoTFactDomain::ClassDefault)
			{
				DefaultsContract.Facts.Add(Fact);
				FWoTRemainingSourceBytesForTesting& Source =
					DefaultSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(
					Source.Bytes, *(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
				if (Fact.Outputs.Num() == 1)
					DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
			}
		}
		if (WorldSources.Num() != 19 || DefaultSources.Num() != 20)
		{
			Error = FString::Printf(TEXT("retail source census was %d worlds/%d defaults"),
				WorldSources.Num(), DefaultSources.Num());
			Test.AddError(Error);
			return false;
		}

		FWoTDecorationPhysicsObjectWriter Writer;
		FWoTDecorationPhysicsProgress Progress;
		TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorlds =
			ImportRequiredWorldSetPackageBytesForTesting(
				WorldSources, WorldContract, Writer, Progress);
		if (ImportedWorlds.IsError())
		{
			Error = ImportedWorlds.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
			ImportRegistryLocalizationDefaultsSourcesForTesting(
				DefaultSources, DefaultsContract, Writer, Progress);
		if (ImportedDefaults.IsError())
		{
			Error = ImportedDefaults.GetError().ToString();
			Test.AddError(Error);
			return false;
		}

		TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
		for (const FString& LogicalId : DefaultLogicalIds)
		{
			TArray<uint8>* Bytes = Writer.Objects.Find(LogicalId);
			if (Bytes == nullptr)
			{
				Error = LogicalId + TEXT(" was not imported");
				Test.AddError(Error);
				return false;
			}
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
					MoveTemp(*Bytes), TEXT("ue1-class-defaults-v2"),
					TEXT("class_default"));
			if (Decoded.IsError())
			{
				Error = Decoded.GetError().ToString();
				Test.AddError(Error);
				return false;
			}
			DefaultObjects.Add(MoveTemp(Decoded).GetValue());
		}
		Defaults = MakeUnique<FWoTRuntimeClassDefaults>();
		if (!Defaults->LoadObjectsForTesting(MoveTemp(DefaultObjects), Error))
		{
			Test.AddError(Error);
			return false;
		}

		for (const FWoTWorldDescriptor& Descriptor : ImportedWorlds.GetValue().Worlds)
		{
			TArray<uint8>* Bytes = Writer.Objects.Find(
				TEXT("cache/world/") + Descriptor.MapIdentity);
			if (Bytes == nullptr)
			{
				Error = Descriptor.MapIdentity + TEXT(" world object was not imported");
				Test.AddError(Error);
				return false;
			}
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
					*Bytes, TEXT("ue1-world-records-le-v6"), TEXT("world"));
			if (Decoded.IsError())
			{
				Error = Decoded.GetError().ToString();
				Test.AddError(Error);
				return false;
			}
			FWoTRuntimeWorldResource Resource;
			const FWoTBootstrapStatus Loaded =
				Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue());
			if (Loaded.IsError())
			{
				Error = Loaded.GetError().ToString();
				Test.AddError(Error);
				return false;
			}
			Worlds.Add(Descriptor.MapIdentity, Resource.Actors());
		}
		if (Worlds.Num() != 19)
		{
			Error = FString::Printf(TEXT("decoded world census was %d maps"), Worlds.Num());
			Test.AddError(Error);
			return false;
		}
		bLoaded = true;
		return true;
	}

	const FWoTRuntimeWorldActor* FindActor(
		FStringView Map, FStringView ActorLeaf) const
	{
		const TArray<FWoTRuntimeWorldActor>* Actors = Worlds.Find(FString(Map));
		const FString WantedLeaf(ActorLeaf);
		return Actors == nullptr ? nullptr : Actors->FindByPredicate(
			[WantedLeaf](const FWoTRuntimeWorldActor& Actor)
			{
				return WoTDecorationPhysicsLeaf(Actor.Identity).Equals(
					WantedLeaf, ESearchCase::IgnoreCase);
			});
	}

	bool bAttempted = false;
	bool bLoaded = false;
	FString Error;
	TUniquePtr<FWoTRuntimeClassDefaults> Defaults;
	TMap<FString, TArray<FWoTRuntimeWorldActor>> Worlds;
};

FWoTDecorationPhysicsRetailFixture& WoTDecorationPhysicsFixture()
{
	static FWoTDecorationPhysicsRetailFixture Fixture;
	return Fixture;
}

bool WoTDecorationPhysicsReadByte(const FWoTRuntimeWorldProperty* Property,
	uint8& Out)
{
	return Property != nullptr && WoTRuntimeWorldPropertyByte(*Property, Out);
}

AWoTDecoration* WoTDecorationPhysicsProjectSingle(
	FAutomationTestBase& Test, UWorld& World,
	const FWoTRuntimeClassDefaults& Defaults,
	const FWoTRuntimeWorldActor& Record, FStringView MapIdentity)
{
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = FString(MapIdentity);
	TArray<FWoTRuntimeWorldActor> Records;
	Records.Add(Record);
	Source.Records = Records;
	FWoTWorldProjectionResult Projection;
	FString Error;
	if (!Test.TestTrue(TEXT("the real decoration projects"),
		FWoTWorldProjector::ProjectDeferred(
			World, Source, Defaults, Projection, Error)))
	{
		if (!Error.IsEmpty()) Test.AddError(Error);
		return nullptr;
	}
	AWoTDecoration* Decoration = nullptr;
	for (AActor* Deferred : Projection.DeferredActors)
	{
		if (AWoTDecoration* Candidate = Cast<AWoTDecoration>(Deferred))
			Decoration = Candidate;
		if (AWoTBreakableDecoration* Breakable =
			Cast<AWoTBreakableDecoration>(Deferred))
		{
			Test.TestFalse(TEXT("authored falling waits for BeginPlay net authority"),
				Breakable->IsFallingLoose());
		}
		if (IsValid(Deferred))
			Deferred->FinishSpawning(Deferred->GetActorTransform(), true);
	}
	Test.TestNotNull(TEXT("the projected actor is a decoration"), Decoration);
	return Decoration;
}

AActor* WoTDecorationPhysicsSpawnFloor(UWorld& World, const FVector& Center)
{
	AActor* Floor = World.SpawnActor<AActor>();
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("DecorationPhysicsFloor"));
	Box->SetBoxExtent(FVector(1000.0f, 1000.0f, 50.0f));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetRootComponent(Box);
	Box->RegisterComponent();
	Floor->SetActorLocation(Center);
	return Floor;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDecorationPhysicsRetailCensusSpec,
	"WoT.World.DecorationPhysics.RealRetailCensus",
	WoTDecorationPhysicsSpecFlags)

bool FWoTDecorationPhysicsRetailCensusSpec::RunTest(const FString&)
{
	FWoTDecorationPhysicsRetailFixture& Fixture = WoTDecorationPhysicsFixture();
	if (!Fixture.Load(*this)) return false;
	const FWoTRuntimeClassDefaults& Defaults = *Fixture.Defaults;

	TArray<FString> RotatingDecorations;
	TArray<FString> FallingDecorations;
	TArray<FString> RotatingZones;
	for (const TPair<FString, TArray<FWoTRuntimeWorldActor>>& Map : Fixture.Worlds)
	{
		for (const FWoTRuntimeWorldActor& Actor : Map.Value)
		{
			uint8 Physics = WoTPhysicsNone;
			const FWoTRuntimeWorldProperty* Property =
				Defaults.FindProperty(Actor, TEXT("Physics"));
			if (Property != nullptr
				&& !WoTDecorationPhysicsReadByte(Property, Physics))
			{
				AddError(Map.Key + TEXT(".") + Actor.Identity
					+ TEXT(" has malformed Physics"));
				continue;
			}
			const FString Label = Map.Key + TEXT(".")
				+ WoTDecorationPhysicsLeaf(Actor.Identity);
			const bool bArenaSix =
				Map.Key.Equals(TEXT("Arena_06"), ESearchCase::IgnoreCase);
			if (Defaults.ClassIsA(Actor.ClassIdentity, TEXT("Engine.Decoration"))
				&& Physics == WoTPhysicsRotating)
			{
				RotatingDecorations.Add(Label);
			}
			if (Defaults.ClassIsA(Actor.ClassIdentity,
					TEXT("WoTDecorations.BarrelTap"))
				&& Physics == WoTPhysicsFalling)
			{
				FallingDecorations.Add(Label);
			}
			else if (bArenaSix && Physics == WoTPhysicsRotating
				&& Defaults.ClassIsA(Actor.ClassIdentity, TEXT("Engine.ZoneInfo")))
			{
				RotatingZones.Add(Label);
			}
		}
	}
	RotatingDecorations.Sort();
	FallingDecorations.Sort();
	RotatingZones.Sort();
	TestEqual(TEXT("exactly four rotating decorations decode"),
		RotatingDecorations.Num(), 4);
	TestEqual(TEXT("the rotating decoration identities"),
		FString::Join(RotatingDecorations, TEXT("|")),
		FString(TEXT("Battle2_01.HangingLight0|Battle2_01.HangingLightChain0|"
			"Battle3_01.HangingLight7|Battle3_01.HangingLightChain7")));
	TestEqual(TEXT("exactly eight falling BarrelTaps decode"),
		FallingDecorations.Num(), 8);
	TestEqual(TEXT("the falling BarrelTap identities"),
		FString::Join(FallingDecorations, TEXT("|")),
		FString(TEXT("Arena_06.BarrelTap0|Arena_06.BarrelTap1|Arena_06.BarrelTap10|"
			"Arena_06.BarrelTap11|Arena_06.BarrelTap6|Arena_06.BarrelTap7|"
			"Arena_06.BarrelTap8|Arena_06.BarrelTap9")));
	TestEqual(TEXT("three rotating zones are excluded from decorations"),
		FString::Join(RotatingZones, TEXT("|")),
		FString(TEXT("Arena_06.WaterZone0|Arena_06.WaterZone1|Arena_06.WaterZone2")));

	const FWoTRuntimeWorldProperty* BarrelDefault =
		Defaults.FindOwnClassProperty(TEXT("WoTDecorations.BarrelTap"), TEXT("Physics"));
	uint8 BarrelDefaultPhysics = WoTPhysicsNone;
	TestTrue(TEXT("BarrelTap class-default Physics decodes from retail bytes"),
		WoTDecorationPhysicsReadByte(BarrelDefault, BarrelDefaultPhysics));
	TestEqual(TEXT("BarrelTap class-default Physics is PHYS_Falling"),
		static_cast<int32>(BarrelDefaultPhysics),
		static_cast<int32>(WoTPhysicsFalling));
	for (const TCHAR* BarrelName : {TEXT("BarrelTap0"), TEXT("BarrelTap1"),
		TEXT("BarrelTap6"), TEXT("BarrelTap7"), TEXT("BarrelTap8"),
		TEXT("BarrelTap9"), TEXT("BarrelTap10"), TEXT("BarrelTap11")})
	{
		const FWoTRuntimeWorldActor* Barrel =
			Fixture.FindActor(TEXT("Arena_06"), BarrelName);
		if (TestNotNull(FString(BarrelName) + TEXT(" decodes"), Barrel))
		{
			TestNull(FString(BarrelName) + TEXT(" has no authored Physics override"),
				WoTRuntimeWorldFindProperty(*Barrel, TEXT("Physics")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDecorationPhysicsRotatingDisplaySpec,
	"WoT.World.DecorationPhysics.RotatingDisplay",
	WoTDecorationPhysicsSpecFlags)

bool FWoTDecorationPhysicsRotatingDisplaySpec::RunTest(const FString&)
{
	FWoTDecorationPhysicsRetailFixture& Fixture = WoTDecorationPhysicsFixture();
	if (!Fixture.Load(*this)) return false;
	const FWoTRuntimeWorldActor* Record =
		Fixture.FindActor(TEXT("Battle2_01"), TEXT("HangingLight0"));
	if (!TestNotNull(TEXT("Battle2_01 HangingLight0 decodes"), Record)) return false;

	uint8 Physics = WoTPhysicsNone;
	bool bFixedRotationDir = false;
	FRotator RotationRate = FRotator::ZeroRotator;
	const FWoTRuntimeWorldProperty* PhysicsProperty =
		WoTRuntimeWorldFindProperty(*Record, TEXT("Physics"));
	const FWoTRuntimeWorldProperty* RateProperty =
		WoTRuntimeWorldFindProperty(*Record, TEXT("RotationRate"));
	const FWoTRuntimeWorldProperty* FixedProperty =
		WoTRuntimeWorldFindProperty(*Record, TEXT("bFixedRotationDir"));
	TestTrue(TEXT("authored Physics decodes from the map record"),
		WoTDecorationPhysicsReadByte(PhysicsProperty, Physics));
	TestTrue(TEXT("authored RotationRate decodes from the map record"),
		RateProperty != nullptr
			&& WoTRuntimeWorldPropertyRotator(*RateProperty, RotationRate));
	TestTrue(TEXT("authored bFixedRotationDir decodes from the map record"),
		FixedProperty != nullptr
			&& WoTRuntimeWorldPropertyBool(*FixedProperty, bFixedRotationDir));
	TestEqual(TEXT("HangingLight0 authors PHYS_Rotating"),
		static_cast<int32>(Physics), static_cast<int32>(WoTPhysicsRotating));
	TestEqual(TEXT("HangingLight0 authors yaw rate 800 UE1 units per second"),
		RotationRate.Yaw, 800.0 * 360.0 / 65536.0, 0.0001);
	TestTrue(TEXT("HangingLight0 fixes its rotation direction"), bFixedRotationDir);

	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("rotation spec world exists"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTDecoration* Decoration = WoTDecorationPhysicsProjectSingle(
		*this, *World, *Fixture.Defaults, *Record, TEXT("Battle2_01"));
	if (!Decoration) return false;
	const FRotator RootBefore = Decoration->GetActorRotation();
	const FRotator DisplayBefore = Decoration->Mesh->GetRelativeRotation();
	constexpr int32 TickCount = 180;
	constexpr float TickStep = 1.0f / 60.0f;
	WoTTest::TickWorld(World, TickCount, TickStep);
	const FRotator RootAfter = Decoration->GetActorRotation();
	const FRotator DisplayAfter = Decoration->Mesh->GetRelativeRotation();
	const double ActualYawAdvance = FRotator::NormalizeAxis(
		DisplayAfter.Yaw - DisplayBefore.Yaw);
	constexpr double ExpectedYawAdvance =
		800.0 * 360.0 / 65536.0 * TickCount * TickStep;
	TestEqual(TEXT("display yaw advances at the decoded retail rate"),
		ActualYawAdvance, ExpectedYawAdvance, 0.02);
	TestTrue(TEXT("the collision actor remains world-axis-aligned"),
		RootAfter.Equals(RootBefore, 0.0001));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDecorationPhysicsFallingRestSpec,
	"WoT.World.DecorationPhysics.FallingRest",
	WoTDecorationPhysicsSpecFlags)

bool FWoTDecorationPhysicsFallingRestSpec::RunTest(const FString&)
{
	FWoTDecorationPhysicsRetailFixture& Fixture = WoTDecorationPhysicsFixture();
	if (!Fixture.Load(*this)) return false;
	const FWoTRuntimeWorldActor* Record =
		Fixture.FindActor(TEXT("Arena_06"), TEXT("BarrelTap0"));
	if (!TestNotNull(TEXT("Arena_06 BarrelTap0 decodes"), Record)) return false;
	TestNull(TEXT("BarrelTap0 has no authored Physics override"),
		WoTRuntimeWorldFindProperty(*Record, TEXT("Physics")));
	uint8 Physics = WoTPhysicsNone;
	TestTrue(TEXT("BarrelTap0 effective Physics decodes through its class default"),
		WoTDecorationPhysicsReadByte(
			Fixture.Defaults->FindProperty(*Record, TEXT("Physics")), Physics));
	TestEqual(TEXT("BarrelTap0 effective Physics is PHYS_Falling"),
		static_cast<int32>(Physics), static_cast<int32>(WoTPhysicsFalling));

	FVector InitialLocation = FVector::ZeroVector;
	const FWoTRuntimeWorldProperty* LocationProperty =
		Fixture.Defaults->FindProperty(*Record, TEXT("Location"));
	if (!TestTrue(TEXT("BarrelTap0 location decodes from retail bytes"),
		LocationProperty != nullptr
			&& WoTRuntimeWorldPropertyVector(*LocationProperty, InitialLocation)))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("falling spec world exists"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	constexpr double DropDistance = 100.0;
	const double FloorTop = InitialLocation.Z - DropDistance;
	WoTDecorationPhysicsSpawnFloor(
		*World, FVector(InitialLocation.X, InitialLocation.Y, FloorTop - 50.0));
	AWoTBreakableDecoration* Barrel = Cast<AWoTBreakableDecoration>(
		WoTDecorationPhysicsProjectSingle(
			*this, *World, *Fixture.Defaults, *Record, TEXT("Arena_06")));
	if (!TestNotNull(TEXT("the real BarrelTap projects as breakable"), Barrel))
		return false;
	TestTrue(TEXT("the placed falling barrel is server authoritative"),
		Barrel->HasAuthority());
	TestTrue(TEXT("breakable movement is replicated to clients"),
		Barrel->GetIsReplicated() && Barrel->IsReplicatingMovement());
	TestTrue(TEXT("PHYS_Falling starts the existing breakable fall path"),
		Barrel->IsFallingLoose());

	const double StartZ = Barrel->GetActorLocation().Z;
	int32 Ticks = 0;
	while (Barrel->IsFallingLoose() && Ticks < 600)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		++Ticks;
	}
	TestTrue(TEXT("the barrel moved down"), Barrel->GetActorLocation().Z < StartZ);
	TestFalse(TEXT("the barrel reaches PHYS_None rest after landing"),
		Barrel->IsFallingLoose());
	TestTrue(TEXT("the cylinder rests on the synthetic floor"),
		FMath::IsNearlyEqual(Barrel->GetActorLocation().Z,
			FloorTop + Barrel->Collision->GetUnscaledCapsuleHalfHeight(), 2.0));
	const FVector RestLocation = Barrel->GetActorLocation();
	WoTTest::TickWorld(World, 60, 1.0f / 60.0f);
	TestTrue(TEXT("the landed barrel remains stopped"),
		Barrel->GetActorLocation().Equals(RestLocation, 0.01));
	return !HasAnyErrors();
}

// The client-mode switch is the editor PIE net-mode seam (UWorld::
// SetPlayInEditorInitialNetMode is WITH_EDITOR only); the native
// WoTGuardedEngineTests target compiles this file without the editor.
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDecorationPhysicsClientStartupSpec,
	"WoT.World.DecorationPhysics.ClientStartupDefersFalling",
	WoTDecorationPhysicsSpecFlags)

bool FWoTDecorationPhysicsClientStartupSpec::RunTest(const FString&)
{
	FWoTDecorationPhysicsRetailFixture& Fixture = WoTDecorationPhysicsFixture();
	if (!Fixture.Load(*this)) return false;
	const FWoTRuntimeWorldActor* Record =
		Fixture.FindActor(TEXT("Arena_06"), TEXT("BarrelTap0"));
	if (!TestNotNull(TEXT("Arena_06 BarrelTap0 decodes"), Record)) return false;

	// Production projects a hidden candidate before the joining client's world
	// context and net driver are installed. Reproduce that ordering: project in
	// standalone mode, then establish client mode immediately before BeginPlay.
	WoTTest::FSpecWorldOptions Options;
	Options.bBeginPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (!TestNotNull(TEXT("pre-BeginPlay client candidate exists"), World))
		return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	TestEqual(TEXT("candidate initially has the materialization-time mode"),
		World->GetNetMode(), NM_Standalone);
	AWoTBreakableDecoration* Barrel = Cast<AWoTBreakableDecoration>(
		WoTDecorationPhysicsProjectSingle(
			*this, *World, *Fixture.Defaults, *Record, TEXT("Arena_06")));
	if (!TestNotNull(TEXT("the real BarrelTap projects as breakable"), Barrel))
		return false;
	TestFalse(TEXT("early projection does not begin local falling"),
		Barrel->IsFallingLoose());
	const FVector ProjectedLocation = Barrel->GetActorLocation();

	World->WorldType = EWorldType::PIE;
	World->SetPlayInEditorInitialNetMode(NM_Client);
	TestEqual(TEXT("the candidate resolves to client mode before BeginPlay"),
		World->GetNetMode(), NM_Client);
	World->BeginPlay();
	TestFalse(TEXT("the joining client does not integrate authored falling"),
		Barrel->IsFallingLoose());
	WoTTest::TickWorld(World, 60, 1.0f / 60.0f);
	TestTrue(TEXT("the client copy waits at its replicated transform"),
		Barrel->GetActorLocation().Equals(ProjectedLocation, 0.01));
	return !HasAnyErrors();
}
#endif // WITH_EDITOR

#endif
