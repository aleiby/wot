// Headless specs for the packaged BSP proof harness's pure seams.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Testing/WoTBSPProofHarness.h"
#include "../WoTBSPPayload.h"
#include "../WoTPayloadSubsystem.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "StaticMeshAttributes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	UStaticMesh* WoTBSPProofTests_MakeMesh(
		const TArray<FName>& Slots, bool bTinyCarrier)
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage());
		if (!Mesh) return nullptr;
		for (FName Slot : Slots)
		{
			// The (interface, slot, imported-slot) constructor is editor-only;
			// this file also compiles into the Development game target.
			FStaticMaterial Material;
			Material.MaterialInterface = nullptr;
			Material.MaterialSlotName = Slot;
#if WITH_EDITORONLY_DATA
			Material.ImportedMaterialSlotName = Slot;
#endif
			Mesh->GetStaticMaterials().Add(Material);
		}
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		auto Positions = Attributes.GetVertexPositions();
		auto Normals = Attributes.GetVertexInstanceNormals();
		auto UVs = Attributes.GetVertexInstanceUVs();
		auto MaterialSlots = Attributes.GetPolygonGroupMaterialSlotNames();
		UVs.SetNumChannels(2);
		for (int32 Section = 0; Section < Slots.Num(); ++Section)
		{
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			MaterialSlots[Group] = Slots[Section];
			FVertexInstanceID Instances[3];
			const float Scale = bTinyCarrier ? 0.1f : 100.0f;
			const FVector3f Points[3] = {
				FVector3f(Section * Scale, 0, 0),
				FVector3f(Section * Scale + Scale, 0, 0),
				FVector3f(Section * Scale, Scale, 0)};
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = Points[Corner];
				Instances[Corner] = Description.CreateVertexInstance(Vertex);
				Normals[Instances[Corner]] = FVector3f(0, 0, 1);
				UVs.Set(Instances[Corner], 0,
					FVector2f(Corner == 1 ? 1.f : 0.f, Corner == 2 ? 1.f : 0.f));
				UVs.Set(Instances[Corner], 1,
					FVector2f(Corner == 1 ? .5f : .25f, Corner == 2 ? .5f : .25f));
			}
			Description.CreateTriangle(Group, MakeArrayView(Instances, 3));
		}
		Mesh->bAllowCPUAccess = true;
		Mesh->NeverStream = true;
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		Params.bBuildSimpleCollision = false;
		Params.bAllowCpuAccess = true;
		const TArray<const FMeshDescription*> Descriptions = {&Description};
		return Mesh->BuildFromMeshDescriptions(Descriptions, Params) ? Mesh : nullptr;
	}

	FString WoTBSPProofTests_ValidPlan()
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("version"), 1);
		Root->SetStringField(TEXT("map"),
			TEXT("/Game/WoT/Maps/Geo/Arena_07/Arena_07"));
		TSharedRef<FJsonObject> AABB = MakeShared<FJsonObject>();
		AABB->SetArrayField(TEXT("min"), {
			MakeShared<FJsonValueNumber>(-100), MakeShared<FJsonValueNumber>(-100),
			MakeShared<FJsonValueNumber>(-100)});
		AABB->SetArrayField(TEXT("max"), {
			MakeShared<FJsonValueNumber>(100), MakeShared<FJsonValueNumber>(100),
			MakeShared<FJsonValueNumber>(100)});
		Root->SetObjectField(TEXT("aabb"), AABB);
		auto MakeProbe = [](const FString& Id)
		{
			TSharedRef<FJsonObject> Probe = MakeShared<FJsonObject>();
			Probe->SetStringField(TEXT("id"), Id);
			Probe->SetArrayField(TEXT("start"), {
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueNumber>(100)});
			Probe->SetArrayField(TEXT("end"), {
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueNumber>(-100)});
			return MakeShared<FJsonValueObject>(Probe);
		};
		TArray<TSharedPtr<FJsonValue>> Floor, Walls, Projectiles, Nav, Cameras;
		for (int32 Index = 0; Index < 128; ++Index)
			Floor.Add(MakeProbe(FString::Printf(TEXT("floor_%03d"), Index)));
		for (int32 Index = 0; Index < 64; ++Index)
			Walls.Add(MakeProbe(FString::Printf(TEXT("wall_%03d"), Index)));
		for (int32 Index = 0; Index < 32; ++Index)
		{
			TSharedPtr<FJsonObject> Shot =
				MakeProbe(FString::Printf(TEXT("projectile_%03d"), Index))->AsObject();
			Shot->SetArrayField(TEXT("origin"), Shot->GetArrayField(TEXT("start")));
			Shot->SetArrayField(TEXT("direction"), {
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueNumber>(-1)});
			Shot->SetStringField(TEXT("class_path"), TEXT("/Script/WoT.WoTDartProjectile"));
			Shot->SetNumberField(TEXT("lifetime_ticks"), 300);
			Shot->SetBoolField(TEXT("portal_directed"), Index >= 28);
			Shot->SetBoolField(TEXT("expect_block"), Index < 28);
			Projectiles.Add(MakeShared<FJsonValueObject>(Shot.ToSharedRef()));
		}
		for (int32 Index = 0; Index < 12; ++Index)
			Nav.Add(MakeProbe(FString::Printf(TEXT("nav_%03d"), Index)));
		for (int32 Index = 0; Index < 5; ++Index)
		{
			TSharedPtr<FJsonObject> Camera =
				MakeProbe(FString::Printf(TEXT("camera_%03d"), Index))->AsObject();
			Camera->SetArrayField(TEXT("location"), Camera->GetArrayField(TEXT("start")));
			Camera->SetArrayField(TEXT("rotation"), {
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueNumber>(0)});
			Cameras.Add(MakeShared<FJsonValueObject>(Camera.ToSharedRef()));
		}
		Root->SetArrayField(TEXT("floor_rays"), Floor);
		Root->SetArrayField(TEXT("wall_rays"), Walls);
		Root->SetArrayField(TEXT("projectiles"), Projectiles);
		Root->SetArrayField(TEXT("nav_pairs"), Nav);
		Root->SetArrayField(TEXT("camera_poses"), Cameras);
		Root->SetNumberField(TEXT("player_start_radial_count"), 8);
		Root->SetNumberField(TEXT("travel_count"), 10);
		TSharedRef<FJsonObject> Pawn = MakeShared<FJsonObject>();
		Pawn->SetStringField(TEXT("class_path"), TEXT("/Script/WoT.WoTAesSedai"));
		Pawn->SetNumberField(TEXT("capsule_radius"), 17);
		Pawn->SetNumberField(TEXT("capsule_half_height"), 39);
		Pawn->SetNumberField(TEXT("settle_seconds"), 5);
		Pawn->SetNumberField(TEXT("floor_bound_z"), -1000);
		Root->SetObjectField(TEXT("pawn"), Pawn);
		TSharedRef<FJsonObject> Seeking = MakeShared<FJsonObject>();
		Seeking->SetStringField(TEXT("class_path"),
			TEXT("/Script/WoT.WoTSeekerProjectile"));
		Seeking->SetArrayField(TEXT("spawn"), {
			MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0),
			MakeShared<FJsonValueNumber>(0)});
		Seeking->SetArrayField(TEXT("target"), {
			MakeShared<FJsonValueNumber>(100), MakeShared<FJsonValueNumber>(0),
			MakeShared<FJsonValueNumber>(0)});
		Seeking->SetArrayField(TEXT("obstruction_center"), {
			MakeShared<FJsonValueNumber>(50), MakeShared<FJsonValueNumber>(0),
			MakeShared<FJsonValueNumber>(0)});
		Root->SetObjectField(TEXT("seeking"), Seeking);
		Root->SetArrayField(TEXT("compact_probe_ids"), {
			MakeShared<FJsonValueString>(TEXT("floor_000")),
			MakeShared<FJsonValueString>(TEXT("projectile_000"))});
		Root->SetArrayField(TEXT("compact_nav_ids"), {
			MakeShared<FJsonValueString>(TEXT("nav_000"))});
		FString Text;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Root, Writer);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPProofPlanSpec,
	"WoT.Payload.BSP.Proof.PlanValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPProofPlanSpec::RunTest(const FString&)
{
	FString Error;
	TestTrue(TEXT("complete deterministic plan parses"),
		UWoTBSPProofSubsystem::ValidatePlanJson(WoTBSPProofTests_ValidPlan(), Error));
	TestFalse(TEXT("malformed JSON fails"),
		UWoTBSPProofSubsystem::ValidatePlanJson(TEXT("{"), Error));
	TestTrue(TEXT("malformed JSON reports context"), !Error.IsEmpty());
	TSharedPtr<FJsonObject> InvalidRoot;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
		WoTBSPProofTests_ValidPlan());
	FJsonSerializer::Deserialize(Reader, InvalidRoot);
	InvalidRoot->SetNumberField(TEXT("version"), 2);
	FString ShortPlan;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ShortPlan);
	FJsonSerializer::Serialize(InvalidRoot.ToSharedRef(), Writer);
	TestFalse(TEXT("unknown plan version fails"),
		UWoTBSPProofSubsystem::ValidatePlanJson(ShortPlan, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPProofFaceSectionSpec,
	"WoT.Payload.BSP.Proof.FaceToSection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPProofFaceSectionSpec::RunTest(const FString&)
{
	UStaticMesh* Mesh = WoTBSPProofTests_MakeMesh({TEXT("First"), TEXT("Second")}, false);
	TestNotNull(TEXT("synthetic mesh builds"), Mesh);
	if (!Mesh) return false;
	TestEqual(TEXT("face zero belongs to first section"),
		UWoTBSPProofSubsystem::ResolveFaceSection(Mesh, 0), 0);
	TestEqual(TEXT("face one belongs to second section"),
		UWoTBSPProofSubsystem::ResolveFaceSection(Mesh, 1), 1);
	TestEqual(TEXT("negative face is unresolved"),
		UWoTBSPProofSubsystem::ResolveFaceSection(Mesh, -1), INDEX_NONE);
	TestEqual(TEXT("past-end face is unresolved"),
		UWoTBSPProofSubsystem::ResolveFaceSection(Mesh, 2), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPProofRenderWriterSpec,
	"WoT.Payload.BSP.Proof.RenderWriterRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPProofRenderWriterSpec::RunTest(const FString&)
{
	UStaticMesh* Mesh = WoTBSPProofTests_MakeMesh({TEXT("Only")}, false);
	TestNotNull(TEXT("synthetic render mesh builds"), Mesh);
	if (!Mesh) return false;
	TSharedPtr<FJsonObject> Dump;
	FString Error;
	TestTrue(TEXT("writer captures all CPU streams"),
		UWoTBSPProofSubsystem::BuildRenderMeshJson(Mesh, Dump, Error));
	if (!Dump.IsValid()) return false;
	TestEqual(TEXT("one LOD"), Dump->GetIntegerField(TEXT("num_lods")), 1);
	TestEqual(TEXT("three positions"),
		Dump->GetArrayField(TEXT("positions")).Num(), 3);
	TestEqual(TEXT("complete index buffer"),
		Dump->GetArrayField(TEXT("indices")).Num(), 3);
	TestTrue(TEXT("CPU readable"), Dump->GetBoolField(TEXT("cpu_readable")));
	return true;
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPProofCounterSpec,
	"WoT.Payload.BSP.Proof.RuntimeCounterMoves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPProofCounterSpec::RunTest(const FString&)
{
	UStaticMesh* Carrier = WoTBSPProofTests_MakeMesh({TEXT("Portal")}, true);
	TestNotNull(TEXT("tiny carrier builds"), Carrier);
	if (!Carrier) return false;
	UWoTBSPPayloadAssetUserData* Binding =
		NewObject<UWoTBSPPayloadAssetUserData>(Carrier);
	Binding->PayloadKey = TEXT("bsp:test__portal");
	Binding->Role = TEXT("portal");
	Binding->VertexCount = 3;
	Binding->IndexCount = 3;
	Binding->BoundsMin = FVector(0, 0, 0);
	Binding->BoundsMax = FVector(100, 100, 0);
	Binding->TangentAlgorithm = TEXT("ue-mikk-preserve-normals-v1");
	Binding->MaterialSlots = {TEXT("Portal")};
	Binding->SectionFirstIndices = {0};
	Binding->SectionIndexCounts = {3};
	Binding->SectionMinVertices = {0};
	Binding->SectionMaxVertices = {2};
	Binding->SectionCollisionEnabled = {false};
	Carrier->AddAssetUserData(Binding);

	TSharedPtr<FWoTBSPPayloadData> Payload = MakeShared<FWoTBSPPayloadData>();
	Payload->bPortal = true;
	Payload->BoundsMin = FVector3f(0, 0, 0);
	Payload->BoundsMax = FVector3f(100, 100, 0);
	Payload->Positions = {FVector3f(0, 0, 0), FVector3f(100, 0, 0),
		FVector3f(0, 100, 0)};
	Payload->Normals = {FVector3f(0, 0, 1), FVector3f(0, 0, 1),
		FVector3f(0, 0, 1)};
	Payload->UV0 = {FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)};
	Payload->UV1 = {FVector2f(.25f, .25f), FVector2f(.5f, .25f),
		FVector2f(.25f, .5f)};
	Payload->Indices = {0, 1, 2};
	FWoTBSPPayloadSection Section;
	Section.MaterialSlot = TEXT("Portal");
	Section.FirstIndex = 0;
	Section.IndexCount = 3;
	Section.MinVertex = 0;
	Section.MaxVertex = 2;
	Section.bCollisionEnabled = false;
	Payload->Sections = {Section};

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	UWoTPayloadSubsystem* Subsystem =
		NewObject<UWoTPayloadSubsystem>(GameInstance);
	const FString CacheDir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectIntermediateDir() / TEXT("BSPProofCounter_")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*CacheDir, true);
	Subsystem->bInjectionEnabled = true;
	Subsystem->bManifestReady = true;
	Subsystem->CacheDirectory = CacheDir;
	FWoTBSPPayloadEntry Entry;
	Entry.CacheKey = Binding->PayloadKey;
	Entry.AssetPath = Carrier->GetPathName();
	Entry.BlobSHA256 = FString::ChrN(64, TEXT('1'));
	Entry.Role = TEXT("portal");
	Entry.TangentAlgorithm = Binding->TangentAlgorithm;
	Entry.VertexCount = 3; Entry.IndexCount = 3; Entry.SectionCount = 1;
	Entry.BoundsMin = Payload->BoundsMin; Entry.BoundsMax = Payload->BoundsMax;
	Entry.MaterialSlots = {TEXT("Portal")};
	Entry.SectionFirstIndices = {0}; Entry.SectionIndexCounts = {3};
	Entry.SectionMinVertices = {0}; Entry.SectionMaxVertices = {2};
	Entry.SectionCollisionEnabled = {false};
	Subsystem->BSPEntries.Add(Entry.CacheKey, Entry);
	Subsystem->LoadedBSPPayloads.Add(Entry.CacheKey, Payload);
	const FWoTBSPProofCounters Before = UWoTPayloadSubsystem::GetBSPProofCounters();
	TestTrue(TEXT("actual portal runtime build succeeds"),
		Subsystem->InjectBSPStaticMesh(Carrier, Entry.CacheKey));
	const FWoTBSPProofCounters After = UWoTPayloadSubsystem::GetBSPProofCounters();
	TestEqual(TEXT("render build counter moves once"),
		After.RenderBuilds, Before.RenderBuilds + 1);
	TestEqual(TEXT("first build has no derived-cache load"),
		After.DerivedLoads, Before.DerivedLoads);
	TestEqual(TEXT("first build writes one derived artifact"),
		After.DerivedWrites, Before.DerivedWrites + 1);
	IFileManager::Get().DeleteDirectory(*CacheDir, false, true);
	// Strip the carrier binding before the transient mesh is GC'd: a later
	// payload spec's subsystem discovery would otherwise find this stale
	// bsp:test__portal carrier and log an identity-mismatch Error, which the
	// automation framework converts into a failure of THAT spec.
	Carrier->RemoveUserDataOfClass(UWoTBSPPayloadAssetUserData::StaticClass());
	Carrier->MarkAsGarbage();
	GameInstance->Shutdown();
	return true;
}
#endif // !WOT_GUARDED_BOOTSTRAP

#endif
