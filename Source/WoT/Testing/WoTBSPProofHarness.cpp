#include "WoTBSPProofHarness.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "../WoTPayloadSubsystem.h"
#include "../World/WoTRuntimeBSPRenderer.h"

#include "NavigationPath.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "AI/Navigation/NavigationDirtyArea.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationSystem.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	const FString MainKey = TEXT("bsp:Arena_07__Arena_07");
	const FString PortalKey = TEXT("bsp:Arena_07__Arena_07_portals");
	const FString ArenaPackage = TEXT("/Game/WoT/Maps/Geo/Arena_07/Arena_07");
	// The LEVEL package (travel target). ArenaPackage above is the world
	// MESH identity used by the plan/render contracts — travelling to it
	// fails with ClientTravelFailure ("Failed to load package").
	const FString ArenaMapPackage = TEXT("/Game/WoT/Maps/Arena_07");
	const FString FrontEndPackage = TEXT("/Game/WoT/Maps/WoTFrontEnd");

	TSharedPtr<FJsonValue> Number(double Value)
	{
		return MakeShared<FJsonValueNumber>(Value);
	}

	TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& Value)
	{
		return {Number(Value.X), Number(Value.Y), Number(Value.Z)};
	}

	TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector3f& Value)
	{
		return {Number(Value.X), Number(Value.Y), Number(Value.Z)};
	}

	TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector2f& Value)
	{
		return {Number(Value.X), Number(Value.Y)};
	}

	bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
		FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values)
			|| !Values || Values->Num() != 3) return false;
		double X = 0, Y = 0, Z = 0;
		if (!(*Values)[0]->TryGetNumber(X) || !(*Values)[1]->TryGetNumber(Y)
			|| !(*Values)[2]->TryGetNumber(Z) || !FMath::IsFinite(X)
			|| !FMath::IsFinite(Y) || !FMath::IsFinite(Z)) return false;
		Out = FVector(X, Y, Z);
		return true;
	}

	bool ValidateArray(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field,
		int32 Minimum, const TCHAR* FirstVector, const TCHAR* SecondVector,
		FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Root->TryGetArrayField(Field, Values) || !Values
			|| Values->Num() < Minimum)
		{
			Error = FString::Printf(TEXT("%s requires at least %d entries"), Field, Minimum);
			return false;
		}
		TSet<FString> Ids;
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*Values)[Index]->AsObject();
			FString Id;
			FVector A, B;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("id"), Id)
				|| Id.IsEmpty() || Ids.Contains(Id)
				|| !ReadVector(Item, FirstVector, A)
				|| (SecondVector && !ReadVector(Item, SecondVector, B)))
			{
				Error = FString::Printf(TEXT("%s[%d] is invalid"), Field, Index);
				return false;
			}
			Ids.Add(Id);
		}
		return true;
	}

	FString ModeName(UWoTBSPProofSubsystem::EMode Mode)
	{
		switch (Mode)
		{
		case UWoTBSPProofSubsystem::EMode::Render: return TEXT("render");
		case UWoTBSPProofSubsystem::EMode::Collision: return TEXT("collision");
		case UWoTBSPProofSubsystem::EMode::Nav: return TEXT("nav");
		case UWoTBSPProofSubsystem::EMode::Travel: return TEXT("travel");
		default: return TEXT("full");
		}
	}

	int32 BSPProofActiveNavTiles(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
		{
			Count += It->GetNumActiveTiles();
		}
		return Count;
	}
}

class FWoTBSPProofLogDevice final : public FOutputDevice
{
public:
	explicit FWoTBSPProofLogDevice(double InBaselineSeconds)
		: BaselineSeconds(InBaselineSeconds)
	{
	}

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity,
		const FName& Category) override
	{
		const FString Line(Message ? Message : TEXT(""));
		if (!Line.Contains(TEXT("WOT-BSP-PROOF"))
			&& Category != FName(TEXT("LogWoTBSPRuntime"))) return;
		FScopeLock Lock(&Mutex);
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("frame"), static_cast<double>(GFrameCounter));
		Item->SetNumberField(TEXT("ms"),
			(FPlatformTime::Seconds() - BaselineSeconds) * 1000.0);
		Item->SetStringField(TEXT("category"), Category.ToString());
		Item->SetStringField(TEXT("verbosity"), ToString(Verbosity));
		Item->SetStringField(TEXT("line"), Line);
		Records.Add(MakeShared<FJsonValueObject>(Item));
	}

	virtual bool CanBeUsedOnAnyThread() const override { return true; }

	TArray<TSharedPtr<FJsonValue>> Snapshot()
	{
		FScopeLock Lock(&Mutex);
		return Records;
	}

private:
	double BaselineSeconds = 0.0;
	FCriticalSection Mutex;
	TArray<TSharedPtr<FJsonValue>> Records;
};

UWoTBSPProofSubsystem::UWoTBSPProofSubsystem(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UWoTBSPProofSubsystem::~UWoTBSPProofSubsystem() = default;

bool UWoTBSPProofSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer);
}

bool UWoTBSPProofSubsystem::ValidatePlanJson(const FString& Text, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("plan is not a JSON object");
		return false;
	}
	int32 Version = 0;
	FString Map;
	const TSharedPtr<FJsonObject>* AABB = nullptr;
	FVector Min, Max;
	if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 1
		|| !Root->TryGetStringField(TEXT("map"), Map) || Map != ArenaPackage
		|| !Root->TryGetObjectField(TEXT("aabb"), AABB) || !AABB
		|| !ReadVector(*AABB, TEXT("min"), Min)
		|| !ReadVector(*AABB, TEXT("max"), Max)
		|| Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
	{
		OutError = TEXT("plan version/map/AABB contract is invalid");
		return false;
	}
	if (!ValidateArray(Root, TEXT("floor_rays"), 128, TEXT("start"), TEXT("end"), OutError)
		|| !ValidateArray(Root, TEXT("wall_rays"), 64, TEXT("start"), TEXT("end"), OutError)
		|| !ValidateArray(Root, TEXT("projectiles"), 32, TEXT("origin"),
			TEXT("direction"), OutError)
		|| !ValidateArray(Root, TEXT("nav_pairs"), 12, TEXT("start"), TEXT("end"), OutError)
		|| !ValidateArray(Root, TEXT("camera_poses"), 5, TEXT("location"),
			TEXT("rotation"), OutError)) return false;
	const TArray<TSharedPtr<FJsonValue>>* ProjectilePlans = nullptr;
	Root->TryGetArrayField(TEXT("projectiles"), ProjectilePlans);
	for (int32 Index = 0; ProjectilePlans && Index < ProjectilePlans->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Shot = (*ProjectilePlans)[Index]->AsObject();
		FString ShotClass;
		int32 Lifetime = 0;
		bool bPortal = false, bExpectBlock = false;
		if (!Shot.IsValid() || !Shot->TryGetStringField(TEXT("class_path"), ShotClass)
			|| ShotClass.IsEmpty()
			|| !Shot->TryGetNumberField(TEXT("lifetime_ticks"), Lifetime)
			|| Lifetime <= 0 || Lifetime > 36000
			|| !Shot->TryGetBoolField(TEXT("portal_directed"), bPortal)
			|| !Shot->TryGetBoolField(TEXT("expect_block"), bExpectBlock)
			// Portal shots must never expect a block; non-portal shots MAY
			// decline the expectation (wall/oblique anchored shots).
			|| (bPortal && bExpectBlock))
		{
			OutError = FString::Printf(TEXT("projectiles[%d] metadata is invalid"), Index);
			return false;
		}
	}
	const TSharedPtr<FJsonObject>* Pawn = nullptr;
	const TSharedPtr<FJsonObject>* Seeking = nullptr;
	FString ClassPath;
	double Radius = 0, HalfHeight = 0, Settle = 0, FloorBound = 0;
	FVector Spawn, Target, ObstructionCenter;
	if (!Root->TryGetObjectField(TEXT("pawn"), Pawn) || !Pawn
		|| !(*Pawn)->TryGetStringField(TEXT("class_path"), ClassPath)
		|| ClassPath.IsEmpty()
		|| !(*Pawn)->TryGetNumberField(TEXT("capsule_radius"), Radius) || Radius <= 0
		|| !(*Pawn)->TryGetNumberField(TEXT("capsule_half_height"), HalfHeight)
		|| HalfHeight < Radius
		|| !(*Pawn)->TryGetNumberField(TEXT("settle_seconds"), Settle) || Settle != 5.0
		|| !(*Pawn)->TryGetNumberField(TEXT("floor_bound_z"), FloorBound)
		|| !FMath::IsFinite(FloorBound)
		|| !Root->TryGetObjectField(TEXT("seeking"), Seeking) || !Seeking
		|| !(*Seeking)->TryGetStringField(TEXT("class_path"), ClassPath)
		|| ClassPath.IsEmpty() || !ReadVector(*Seeking, TEXT("spawn"), Spawn)
		|| !ReadVector(*Seeking, TEXT("target"), Target)
		|| !ReadVector(*Seeking, TEXT("obstruction_center"), ObstructionCenter))
	{
		OutError = TEXT("plan pawn/seeking contract is invalid");
		return false;
	}
	int32 PlanTravelCount = 0;
	if (!Root->TryGetNumberField(TEXT("travel_count"), PlanTravelCount)
		|| PlanTravelCount <= 0 || PlanTravelCount > 100)
	{
		OutError = TEXT("travel_count must be in [1,100]");
		return false;
	}
	int32 RadialCount = 0;
	if (!Root->TryGetNumberField(TEXT("player_start_radial_count"), RadialCount)
		|| RadialCount < 8)
	{
		OutError = TEXT("player_start_radial_count must be at least eight");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* CompactProbes = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* CompactNav = nullptr;
	if (!Root->TryGetArrayField(TEXT("compact_probe_ids"), CompactProbes)
		|| !CompactProbes || CompactProbes->IsEmpty()
		|| !Root->TryGetArrayField(TEXT("compact_nav_ids"), CompactNav)
		|| !CompactNav || CompactNav->IsEmpty())
	{
		OutError = TEXT("plan compact subsets are absent");
		return false;
	}
	OutError.Reset();
	return true;
}

int32 UWoTBSPProofSubsystem::ResolveFaceSection(
	const UStaticMesh* Mesh, int32 FaceIndex)
{
	if (!Mesh || FaceIndex < 0 || !Mesh->GetRenderData()
		|| !Mesh->GetRenderData()->LODResources.IsValidIndex(0)) return INDEX_NONE;
	const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
	const uint64 FirstIndex = static_cast<uint64>(FaceIndex) * 3u;
	for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
	{
		if (!LOD.Sections.IsValidIndex(SectionIndex)) continue;
		const FStaticMeshSection& Section = LOD.Sections[SectionIndex];
		const uint64 End = static_cast<uint64>(Section.FirstIndex)
			+ static_cast<uint64>(Section.NumTriangles) * 3u;
		if (FirstIndex >= Section.FirstIndex && FirstIndex < End) return SectionIndex;
	}
	return INDEX_NONE;
}

bool UWoTBSPProofSubsystem::BuildRenderMeshJson(UStaticMesh* Mesh,
	TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	OutJson.Reset();
	if (!Mesh || !Mesh->GetRenderData()
		|| !Mesh->GetRenderData()->LODResources.IsValidIndex(0))
	{
		OutError = TEXT("static mesh has no loaded LOD0 render data");
		return false;
	}
	const FStaticMeshRenderData* Render = Mesh->GetRenderData();
	const FStaticMeshLODResources& LOD = Render->LODResources[0];
	const uint32 PositionCount = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	const uint32 StaticCount = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices();
	TArray<uint32> Indices;
	LOD.IndexBuffer.GetCopy(Indices);
	const bool bReadable = PositionCount > 0 && StaticCount == PositionCount
		&& LOD.IndexBuffer.GetNumIndices() > 0
		&& Indices.Num() == static_cast<int32>(LOD.IndexBuffer.GetNumIndices());
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("object_path"), Mesh->GetPathName());
	Root->SetNumberField(TEXT("num_lods"), Render->LODResources.Num());
	Root->SetNumberField(TEXT("uv_channels"),
		LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());
	Root->SetBoolField(TEXT("allow_cpu_access"), Mesh->bAllowCPUAccess);
	Root->SetBoolField(TEXT("never_stream"), Mesh->NeverStream);
	Root->SetBoolField(TEXT("nanite_valid"), Mesh->HasValidNaniteData());
	Root->SetBoolField(TEXT("cpu_readable"), bReadable);
	TArray<TSharedPtr<FJsonValue>> Materials;
	for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("slot_name"), Material.MaterialSlotName.ToString());
		Item->SetStringField(TEXT("material_path"), Material.MaterialInterface
			? Material.MaterialInterface->GetPathName() : FString());
		Materials.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("materials"), Materials);
	TArray<TSharedPtr<FJsonValue>> Sections;
	int64 TriangleTotal = 0;
	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("material_index"), Section.MaterialIndex);
		Item->SetNumberField(TEXT("first_index"), Section.FirstIndex);
		Item->SetNumberField(TEXT("num_triangles"), Section.NumTriangles);
		Item->SetNumberField(TEXT("min_vertex_index"), Section.MinVertexIndex);
		Item->SetNumberField(TEXT("max_vertex_index"), Section.MaxVertexIndex);
		Item->SetBoolField(TEXT("enable_collision"), Section.bEnableCollision);
		Sections.Add(MakeShared<FJsonValueObject>(Item));
		TriangleTotal += Section.NumTriangles;
	}
	Root->SetArrayField(TEXT("sections"), Sections);
	TArray<TSharedPtr<FJsonValue>> Positions, Normals, Tangents, Signs, UV0, UV1;
	// GetVertexUV asserts (fatally) past the buffer's real channel count, so
	// a channel-starved mesh must fail this dump cleanly instead of crashing.
	if (bReadable
		&& LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() < 2)
	{
		OutError = FString::Printf(
			TEXT("%s LOD0 has %u UV channels; the proof dump requires 2"),
			*Mesh->GetPathName(),
			LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());
		return false;
	}
	if (bReadable)
	{
		Positions.Reserve(PositionCount); Normals.Reserve(PositionCount);
		Tangents.Reserve(PositionCount); Signs.Reserve(PositionCount);
		UV0.Reserve(PositionCount); UV1.Reserve(PositionCount);
		for (uint32 Vertex = 0; Vertex < PositionCount; ++Vertex)
		{
			const FVector3f Position =
				LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex);
			const FVector3f TangentX =
				LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Vertex);
			const FVector3f TangentY =
				LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Vertex);
			const FVector3f Normal =
				LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Vertex);
			const float Sign = FVector3f::DotProduct(
				FVector3f::CrossProduct(Normal, TangentX), TangentY) < 0.f ? -1.f : 1.f;
			Positions.Add(MakeShared<FJsonValueArray>(VectorJson(Position)));
			Normals.Add(MakeShared<FJsonValueArray>(VectorJson(Normal)));
			Tangents.Add(MakeShared<FJsonValueArray>(VectorJson(TangentX)));
			Signs.Add(Number(Sign));
			UV0.Add(MakeShared<FJsonValueArray>(VectorJson(
				LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, 0))));
			UV1.Add(MakeShared<FJsonValueArray>(VectorJson(
				LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, 1))));
		}
	}
	Root->SetArrayField(TEXT("positions"), Positions);
	Root->SetArrayField(TEXT("normals"), Normals);
	Root->SetArrayField(TEXT("tangent_x"), Tangents);
	Root->SetArrayField(TEXT("binormal_sign"), Signs);
	Root->SetArrayField(TEXT("uv0"), UV0);
	Root->SetArrayField(TEXT("uv1"), UV1);
	TArray<TSharedPtr<FJsonValue>> IndexValues;
	for (uint32 Index : Indices) IndexValues.Add(Number(Index));
	Root->SetArrayField(TEXT("indices"), IndexValues);
	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
	BoundsJson->SetArrayField(TEXT("box_min"), VectorJson(Bounds.GetBox().Min));
	BoundsJson->SetArrayField(TEXT("box_max"), VectorJson(Bounds.GetBox().Max));
	BoundsJson->SetArrayField(TEXT("sphere_origin"), VectorJson(Bounds.Origin));
	BoundsJson->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
	Root->SetObjectField(TEXT("bounds"), BoundsJson);
	Root->SetNumberField(TEXT("triangle_count"), TriangleTotal);
	OutJson = Root;
	if (!bReadable)
	{
		OutError = TEXT("LOD0 CPU buffers are not readable");
		return false;
	}
	OutError.Reset();
	return true;
}

void UWoTBSPProofSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	const double InitializeBaselineSeconds = FPlatformTime::Seconds();
	Super::Initialize(Collection);
	if (!FParse::Value(FCommandLine::Get(), TEXT("wotbspproof="), OutDir)) return;
	bActive = true;
	UE_LOG(LogTemp, Display, TEXT("WOT-BSP-PROOF-INIT out=%s"), *OutDir);
	if (FPaths::IsRelative(OutDir))
	{
		Fail(TEXT("proof output path must be absolute"));
		return;
	}
	IFileManager::Get().MakeDirectory(*OutDir, true);
	IFileManager::Get().MakeDirectory(*(OutDir / TEXT("shots")), true);
	LogDevice = new FWoTBSPProofLogDevice(InitializeBaselineSeconds);
	if (GLog) GLog->AddOutputDevice(LogDevice);
	if (!FParse::Value(FCommandLine::Get(), TEXT("wotbspproofplan="), PlanPath)
		|| FPaths::IsRelative(PlanPath))
	{
		Fail(TEXT("proof plan path must be absolute"));
		return;
	}
	FString ModeString = TEXT("full");
	FParse::Value(FCommandLine::Get(), TEXT("wotbspproofmode="), ModeString);
	if (ModeString == TEXT("render")) Mode = EMode::Render;
	else if (ModeString == TEXT("collision")) Mode = EMode::Collision;
	else if (ModeString == TEXT("nav")) Mode = EMode::Nav;
	else if (ModeString == TEXT("travel")) Mode = EMode::Travel;
	else if (ModeString != TEXT("full"))
	{
		Fail(FString::Printf(TEXT("unknown proof mode '%s'"), *ModeString));
		return;
	}
	FParse::Value(FCommandLine::Get(), TEXT("wotbspprooftravels="), TravelCount);
	if (TravelCount <= 0 || TravelCount > 100)
	{
		Fail(TEXT("wotbspprooftravels must be in [1,100]")); return;
	}
	FString Text, Error;
	if (!FFileHelper::LoadFileToString(Text, *PlanPath)
		|| !ValidatePlanJson(Text, Error))
	{
		Fail(Error.IsEmpty() ? TEXT("failed to read proof plan") : Error); return;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	FJsonSerializer::Deserialize(Reader, Plan);
	WorldChangedHandle = FWorldDelegates::OnGameInstanceWorldChanged.AddUObject(
		this, &UWoTBSPProofSubsystem::HandleGameInstanceWorldChanged);
	WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddUObject(
		this, &UWoTBSPProofSubsystem::HandleWorldTickStart);
	State = EState::WaitingForArena;
	if (GetGameInstance()) BindWorld(GetGameInstance()->GetWorld());
}

void UWoTBSPProofSubsystem::Deinitialize()
{
	if (bActive)
	{
		FWorldDelegates::OnGameInstanceWorldChanged.Remove(WorldChangedHandle);
		FWorldDelegates::OnWorldTickStart.Remove(WorldTickHandle);
		if (ImpactProbeHandle.IsValid())
		{
			AWoTGenericProjectile::OnImpactProbe.Remove(ImpactProbeHandle);
			ImpactProbeHandle.Reset();
		}
		if (BoundWorld.IsValid())
			BoundWorld->OnWorldBeginPlay.Remove(WorldBeginPlayHandle);
		WriteProofLog();
		if (GLog && LogDevice) GLog->RemoveOutputDevice(LogDevice);
		delete LogDevice;
		LogDevice = nullptr;
	}
	Super::Deinitialize();
}

void UWoTBSPProofSubsystem::HandleGameInstanceWorldChanged(
	UGameInstance* GameInstance, UWorld* OldWorld, UWorld* NewWorld)
{
	if (!bActive || GameInstance != GetGameInstance()) return;
	if (OldWorld) OldWorld->OnWorldBeginPlay.Remove(WorldBeginPlayHandle);
	BindWorld(NewWorld);
}

void UWoTBSPProofSubsystem::BindWorld(UWorld* World)
{
	BoundWorld = World;
	WorldBeginPlayHandle.Reset();
	if (!World || !World->IsGameWorld()) return;
	WorldBeginPlayHandle = World->OnWorldBeginPlay.AddUObject(
		this, &UWoTBSPProofSubsystem::HandleWorldBeginPlay);
	if (World->HasBegunPlay()) HandleWorldBeginPlay();
}

void UWoTBSPProofSubsystem::HandleWorldBeginPlay()
{
	UWorld* World = BoundWorld.Get();
	if (!World || World->GetGameInstance() != GetGameInstance()) return;
	const FString Package = World->GetOutermost()->GetName();
	if (Package.EndsWith(TEXT("/Arena_07")))
	{
		BeginArenaRun(World);
	}
	else if (Package.EndsWith(TEXT("/WoTFrontEnd"))
		&& State == EState::TravelToFrontEnd)
	{
		State = EState::TravelToArena;
		WaitTicks = 0;
	}
}

void UWoTBSPProofSubsystem::BeginArenaRun(UWorld* World)
{
	BoundWorld = World;
	WaitTicks = 0;
	bNavObservedActive = false;
	NavEpochInitialTileCount = BSPProofActiveNavTiles(World);
	UWoTPayloadSubsystem* Payloads = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (Payloads && Payloads->HasSeenBSPCarrier())
	{
		State = EState::WaitingForReadiness;
	}
	else
	{
		State = EState::ReferenceSettle;
	}
}

void UWoTBSPProofSubsystem::HandleWorldTickStart(
	UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	(void)TickType;
	if (!bActive || !World || World != BoundWorld.Get()
		|| World->GetGameInstance() != GetGameInstance()) return;
	UNavigationSystemV1* CurrentNav =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	UWoTPayloadSubsystem* CurrentPayloads = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	const int32 CurrentNavTiles = BSPProofActiveNavTiles(World);
	// Dirty-area bookkeeping never drains on a live map (movers, rotators and
	// pawns re-dirty nav every tick), so the idle gate keys ONLY off actual
	// tile building; dirtiness still counts as evidence of an active epoch.
	const bool bNavBuilding =
		CurrentNav && CurrentNav->IsNavigationBuildInProgress();
	const bool bNavBusy = bNavBuilding
		|| (CurrentNav && (CurrentNav->IsNavigationDirty()
			|| CurrentNav->HasDirtyAreasQueued()));
	if (CurrentNav && CurrentPayloads)
	{
		if (CurrentPayloads->HasSeenBSPCarrier())
		{
			if (CurrentPayloads->IsBSPKeyReady(MainKey)
				&& (bNavBusy || CurrentNavTiles != NavEpochInitialTileCount))
			{
				bNavObservedActive = true;
			}
		}
		else if (bNavBusy || CurrentNavTiles > 0)
		{
			// A may finish its load-time generation before the first proof tick;
			// an initially idle reference is accepted only with real active tiles.
			bNavObservedActive = true;
		}
	}
	if (State == EState::WaitingForReadiness)
	{
		UWoTPayloadSubsystem* Payloads = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
		if (!Payloads) { Fail(TEXT("payload subsystem is absent")); return; }
		if (Payloads->HasBSPKeyFailed(MainKey) || Payloads->HasBSPKeyFailed(PortalKey))
		{
			Fail(TEXT("required BSP key entered failed state")); return;
		}
		if (Payloads->IsBSPKeyReady(MainKey) && Payloads->IsBSPKeyReady(PortalKey)
			&& !World->IsPaused()) AdvanceReadyRun();
		return;
	}
	if (State == EState::ReferenceSettle)
	{
		if (++WaitTicks >= 60) AdvanceReadyRun();
		return;
	}
	if (State == EState::Screenshots)
	{
		if (++ScreenshotWaitTicks >= 2) StepScreenshot();
		return;
	}
	if (State == EState::CollisionSettle)
	{
		SettleGameSeconds += FMath::Max(0.0f, DeltaSeconds);
		if (SettleGameSeconds >= 5.0) FinishPawnSettle();
		return;
	}
	if (State == EState::CollisionProjectiles
		|| State == EState::TravelCompactProjectiles)
	{
		for (TPair<TWeakObjectPtr<AActor>, int32>& Pair : ProjectileExpiryTicks)
		{
			if (Pair.Value > 0 && --Pair.Value == 0 && Pair.Key.IsValid())
				Pair.Key->Destroy();
		}
		if (--ProjectileTicksRemaining <= 0) FinishProjectiles();
		return;
	}
	if (State == EState::NavWait)
	{
		++NavWaitTicks;
		// Quiescence = an observed generation epoch plus 30 consecutive ticks
		// with no tile build in flight. Hard timeout keeps this fail-closed
		// instead of hanging the packaged process.
		if (bNavBuilding) NavQuietTicks = 0;
		else if (CurrentNav && bNavObservedActive && ++NavQuietTicks >= 30)
		{
			StartNav(bNavCompact);
			return;
		}
		if (NavWaitTicks > 3600)
		{
			Fail(TEXT("navigation generation never reached quiescence"));
		}
		return;
	}
	if (State == EState::NavSeeking)
	{
		if (SeekingProjectile.IsValid())
		{
			LastSeekingPosition = SeekingProjectile->GetActorLocation();
			SeekingTrajectory.Add(MakeShared<FJsonValueArray>(
				VectorJson(LastSeekingPosition)));
			if (SeekingSideSign == 0)
			{
				const FVector TrajectoryVector = LastSeekingPosition - SeekingSpawnPosition;
				const double Cross = SeekingObstructionVector.X *
					TrajectoryVector.Y - SeekingObstructionVector.Y * TrajectoryVector.X;
				if (FMath::Abs(Cross) > UE_DOUBLE_KINDA_SMALL_NUMBER)
					SeekingSideSign = Cross > 0.0 ? 1 : -1;
			}
			if (FVector::DistSquared(LastSeekingPosition, SeekingTargetPosition)
				<= FMath::Square(25.0))
			{
				FinishNavSeeking(TEXT("reached"));
				return;
			}
		}
		else
		{
			FinishNavSeeking(bSeekingDestroyedAtTarget || FVector::DistSquared(
				LastSeekingPosition, SeekingTargetPosition) <= FMath::Square(25.0)
				? TEXT("reached") : TEXT("destroyed"));
			return;
		}
		if (--NavSeekingTicksRemaining <= 0) FinishNavSeeking(TEXT("timeout"));
		return;
	}
	if (State == EState::TravelToArena && ++WaitTicks >= 1)
	{
		if ((TravelIndex & 1) != 0) CollectGarbage(RF_NoFlags, true);
		State = EState::WaitingForArena;
		bReturningToArena = true;
		UGameplayStatics::OpenLevel(World, FName(*ArenaMapPackage), true);
	}
}

void UWoTBSPProofSubsystem::AdvanceReadyRun()
{
	if (Mode == EMode::Travel)
	{
		if (bReturningToArena) StartCollision(true);
		else BeginTravel();
	}
	else if (Mode == EMode::Render || Mode == EMode::Full) StartRender();
	else if (Mode == EMode::Collision) StartCollision(false);
	else BeginNavWait(false);
}

bool UWoTBSPProofSubsystem::WriteJson(const FString& Filename,
	const TSharedRef<FJsonObject>& Object, FString& OutError) const
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object, Writer)
		|| !FFileHelper::SaveStringToFile(Text + TEXT("\n"), *Filename,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("failed to write %s"), *Filename);
		return false;
	}
	return true;
}

bool UWoTBSPProofSubsystem::DumpRenderRole(const FString& Role,
	const FString& DirectoryPrefix, const FString& Filename)
{
	TArray<UStaticMesh*> Meshes;
	for (TObjectIterator<UStaticMesh> It; It; ++It)
	{
		UStaticMesh* Candidate = *It;
		if (!IsValid(Candidate) || Candidate->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue;
		}
		// Only meshes DIRECTLY in the role directory are BSP payload subjects.
		// Subdirectories under the main Geo folder (Movers/<Model>/...) are
		// mover meshes with their own UV layouts — never part of the gate, and
		// reading UV1 on their single-channel buffers trips an engine assert.
		const FString Path = Candidate->GetPathName();
		if (Path.StartsWith(DirectoryPrefix)
			&& !Path.Mid(DirectoryPrefix.Len()).Contains(TEXT("/")))
		{
			Meshes.Add(Candidate);
		}
	}
	Meshes.Sort([](const UStaticMesh& A, const UStaticMesh& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	if (Meshes.IsEmpty()) { Fail(FString::Printf(TEXT("no loaded %s meshes"), *Role)); return false; }
	TArray<TSharedPtr<FJsonValue>> Values;
	for (UStaticMesh* Candidate : Meshes)
	{
		UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *Candidate->GetPathName());
		TSharedPtr<FJsonObject> MeshJson;
		FString Error;
		if (!Mesh || !BuildRenderMeshJson(Mesh, MeshJson, Error))
		{
			Fail(Error.IsEmpty() ? TEXT("loaded mesh identity was not resolvable") : Error);
			return false;
		}
		Values.Add(MakeShared<FJsonValueObject>(MeshJson.ToSharedRef()));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("role"), Role);
	Root->SetArrayField(TEXT("meshes"), Values);
	FString Error;
	if (!WriteJson(Filename, Root, Error)) { Fail(Error); return false; }
	return true;
}

void UWoTBSPProofSubsystem::StartRender()
{
	State = EState::RenderDump;
	if (!DumpRenderRole(TEXT("main"), TEXT("/Game/WoT/Maps/Geo/Arena_07/"),
		OutDir / TEXT("render_main.json"))) return;
	if (!DumpRenderRole(TEXT("portal"), TEXT("/Game/WoT/Maps/Geo/Arena_07_portals/"),
		OutDir / TEXT("render_portal.json"))) return;
	UWorld* World = BoundWorld.Get();
	if (!World) { Fail(TEXT("world disappeared before screenshots")); return; }
	Camera = World->SpawnActor<ACameraActor>();
	if (!Camera.IsValid()) { Fail(TEXT("failed to spawn screenshot camera")); return; }
	if (APlayerController* PC = World->GetFirstPlayerController()) PC->SetViewTarget(Camera.Get());
	ScreenshotIndex = 0; ScreenshotWaitTicks = 2; State = EState::Screenshots;
}

void UWoTBSPProofSubsystem::StepScreenshot()
{
	ScreenshotWaitTicks = 0;
	const TArray<TSharedPtr<FJsonValue>>* Poses = nullptr;
	if (!Plan->TryGetArrayField(TEXT("camera_poses"), Poses) || !Poses)
	{
		Fail(TEXT("camera poses disappeared from plan")); return;
	}
	if (ScreenshotIndex >= Poses->Num())
	{
		if (Mode == EMode::Render) FinishSuccess(); else StartCollision(false);
		return;
	}
	const TSharedPtr<FJsonObject> Pose = (*Poses)[ScreenshotIndex]->AsObject();
	FVector Location, RotationVector;
	FString Id;
	double FOV = 90.0;
	if (!Pose.IsValid() || !Pose->TryGetStringField(TEXT("id"), Id)
		|| !ReadVector(Pose, TEXT("location"), Location)
		|| !ReadVector(Pose, TEXT("rotation"), RotationVector))
	{
		Fail(TEXT("invalid screenshot pose")); return;
	}
	Pose->TryGetNumberField(TEXT("fov"), FOV);
	Camera->SetActorLocationAndRotation(Location,
		FRotator(RotationVector.X, RotationVector.Y, RotationVector.Z));
	Camera->GetCameraComponent()->SetFieldOfView(FOV);
	FScreenshotRequest::RequestScreenshot(OutDir / TEXT("shots") / (Id + TEXT(".png")),
		false, false);
	++ScreenshotIndex;
}

TSharedPtr<FJsonObject> UWoTBSPProofSubsystem::HitJson(const FString& Id,
	const FString& Kind, const FVector& Start, const FVector& End,
	const FHitResult& Hit) const
{
	TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("id"), Id); Item->SetStringField(TEXT("kind"), Kind);
	Item->SetArrayField(TEXT("start"), VectorJson(Start));
	Item->SetArrayField(TEXT("end"), VectorJson(End));
	Item->SetBoolField(TEXT("blocking_hit"), Hit.bBlockingHit);
	Item->SetStringField(TEXT("actor"), Hit.GetActor() ? Hit.GetActor()->GetName() : FString());
	Item->SetStringField(TEXT("component"), Hit.GetComponent()
		? Hit.GetComponent()->GetName() : FString());
	FString Slot;
	if (const AWoTRuntimeBSPWorld* BSPWorld =
		Cast<AWoTRuntimeBSPWorld>(Hit.GetActor()))
	{
		uint32 SurfaceIndex = MAX_uint32;
		FString TextureIdentity;
		if (BSPWorld->ResolveInvisibleSolidCollisionFace(Hit.GetComponent(),
			Hit.FaceIndex, SurfaceIndex, TextureIdentity))
		{
			Slot = WoTRuntimeBSPConverterSlotBase(TextureIdentity);
		}
	}
	if (const UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(Hit.GetComponent()))
	{
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		Item->SetStringField(TEXT("mesh_path"),
			Mesh ? Mesh->GetPathName() : FString());
		const int32 Section = Slot.IsEmpty()
			? ResolveFaceSection(Mesh, Hit.FaceIndex) : INDEX_NONE;
		if (Slot.IsEmpty() && Mesh && Mesh->GetRenderData()
			&& Mesh->GetRenderData()->LODResources.IsValidIndex(0)
			&& Mesh->GetRenderData()->LODResources[0].Sections.IsValidIndex(Section))
		{
			const int32 MaterialIndex = Mesh->GetRenderData()->LODResources[0]
				.Sections[Section].MaterialIndex;
			if (Mesh->GetStaticMaterials().IsValidIndex(MaterialIndex))
				Slot = Mesh->GetStaticMaterials()[MaterialIndex].MaterialSlotName.ToString();
		}
	}
	Item->SetStringField(TEXT("material_slot"), Slot);
	Item->SetArrayField(TEXT("impact_point"), VectorJson(Hit.ImpactPoint));
	Item->SetArrayField(TEXT("impact_normal"), VectorJson(Hit.ImpactNormal));
	Item->SetNumberField(TEXT("distance"), Hit.Distance);
	return Item;
}

void UWoTBSPProofSubsystem::StartCollision(bool bCompact)
{
	bCompactRun = bCompact;
	ProbeRecords.Reset(); SettleRecords.Reset(); ProjectileRecords.Reset();
	ProjectileRecordIndices.Reset(); ProjectileExpiryTicks.Reset();
	SettlingPawns.Reset(); SettlingPawnIds.Reset();
	UWorld* World = BoundWorld.Get();
	if (!World) { Fail(TEXT("collision world disappeared")); return; }
	TSet<FString> CompactIds;
	if (bCompact)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		Plan->TryGetArrayField(TEXT("compact_probe_ids"), Values);
		if (Values) for (const TSharedPtr<FJsonValue>& Value : *Values)
			CompactIds.Add(Value->AsString());
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTBSPProof), true);
	Params.bReturnFaceIndex = true;
	auto RunLines = [&](const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		Plan->TryGetArrayField(Field, Values);
		if (!Values) return;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Probe = Value->AsObject();
			FString Id; FVector Start, End;
			Probe->TryGetStringField(TEXT("id"), Id);
			if (bCompact && !CompactIds.Contains(Id)) continue;
			ReadVector(Probe, TEXT("start"), Start); ReadVector(Probe, TEXT("end"), End);
			FHitResult Hit;
			World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
			ProbeRecords.Add(MakeShared<FJsonValueObject>(
				HitJson(Id, TEXT("line"), Start, End, Hit).ToSharedRef()));
		}
	};
	RunLines(TEXT("floor_rays")); RunLines(TEXT("wall_rays"));
	if (!bCompact)
	{
		const TSharedPtr<FJsonObject>* PawnPlan = nullptr;
		Plan->TryGetObjectField(TEXT("pawn"), PawnPlan);
		double Radius = 17, HalfHeight = 39, FloorBound = -10000;
		FString PawnClassPath;
		(*PawnPlan)->TryGetNumberField(TEXT("capsule_radius"), Radius);
		(*PawnPlan)->TryGetNumberField(TEXT("capsule_half_height"), HalfHeight);
		(*PawnPlan)->TryGetNumberField(TEXT("floor_bound_z"), FloorBound);
		(*PawnPlan)->TryGetStringField(TEXT("class_path"), PawnClassPath);
		TArray<APlayerStart*> Starts;
		for (TActorIterator<APlayerStart> It(World); It; ++It) Starts.Add(*It);
		Starts.Sort([](const APlayerStart& A, const APlayerStart& B)
		{
			const FVector LA = A.GetActorLocation(), LB = B.GetActorLocation();
			return LA.X != LB.X ? LA.X < LB.X : (LA.Y != LB.Y ? LA.Y < LB.Y : LA.Z < LB.Z);
		});
		if (Starts.IsEmpty())
		{
			Fail(TEXT("collision capture found zero PlayerStarts"));
			return;
		}
		UClass* PawnClass = LoadObject<UClass>(nullptr, *PawnClassPath);
		for (int32 Index = 0; Index < Starts.Num(); ++Index)
		{
			const FVector Location = Starts[Index]->GetActorLocation();
			const FVector SweepStart = Location + FVector(0, 0, HalfHeight * 2.0);
			const FVector SweepEnd(Location.X, Location.Y, FloorBound);
			FHitResult SweepHit;
			World->SweepSingleByObjectType(SweepHit, SweepStart, SweepEnd,
				FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
				FCollisionShape::MakeCapsule(Radius, HalfHeight), Params);
			TSharedPtr<FJsonObject> Capsule = HitJson(
				FString::Printf(TEXT("start_%03d_capsule"), Index), TEXT("capsule"),
				SweepStart, SweepEnd, SweepHit);
			Capsule->SetNumberField(TEXT("capsule_radius"), Radius);
			Capsule->SetNumberField(TEXT("capsule_half_height"), HalfHeight);
			ProbeRecords.Add(MakeShared<FJsonValueObject>(Capsule.ToSharedRef()));
			for (int32 Ray = 0; Ray < 8; ++Ray)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Ray / 8.0;
				const FVector End = Location + FVector(FMath::Cos(Angle),
					FMath::Sin(Angle), 0) * 1024.0;
				FHitResult Hit;
				World->LineTraceSingleByChannel(Hit, Location, End, ECC_Visibility, Params);
				ProbeRecords.Add(MakeShared<FJsonValueObject>(HitJson(
					FString::Printf(TEXT("start_%03d_radial_%d"), Index, Ray),
					TEXT("line"), Location, End, Hit).ToSharedRef()));
			}
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			const FRotator StartRotation = Starts[Index]->GetActorRotation();
			AActor* Pawn = PawnClass ? World->SpawnActor<AActor>(
				PawnClass, Location, StartRotation, SpawnParams) : nullptr;
			if (!Pawn) { Fail(TEXT("failed to spawn gameplay pawn")); return; }
			SettlingPawns.Add(Pawn);
			SettlingPawnIds.Add(FString::Printf(TEXT("start_%03d"), Index));
		}
		SettleGameSeconds = 0.0; State = EState::CollisionSettle;
		return;
	}
	StartProjectiles(true);
}

void UWoTBSPProofSubsystem::FinishPawnSettle()
{
	const TSharedPtr<FJsonObject>* PawnPlan = nullptr;
	Plan->TryGetObjectField(TEXT("pawn"), PawnPlan);
	double FloorBound = -10000;
	(*PawnPlan)->TryGetNumberField(TEXT("floor_bound_z"), FloorBound);
	for (int32 Index = 0; Index < SettlingPawns.Num(); ++Index)
	{
		AActor* Actor = SettlingPawns[Index].Get();
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("id"), SettlingPawnIds.IsValidIndex(Index)
			? SettlingPawnIds[Index] : FString::Printf(TEXT("start_%03d"), Index));
		Item->SetNumberField(TEXT("settled_z"), Actor ? Actor->GetActorLocation().Z : -DBL_MAX);
		const ACharacter* Character = Cast<ACharacter>(Actor);
		Item->SetStringField(TEXT("movement_mode"), Character && Character->GetCharacterMovement()
			? Character->GetCharacterMovement()->GetMovementName() : FString());
		Item->SetBoolField(TEXT("fall_through"), !Actor || Actor->GetActorLocation().Z < FloorBound);
		SettleRecords.Add(MakeShared<FJsonValueObject>(Item));
		if (Actor) Actor->Destroy();
	}
	SettlingPawns.Reset(); SettlingPawnIds.Reset();
	StartProjectiles(false);
}

void UWoTBSPProofSubsystem::StartProjectiles(bool bCompact)
{
	UWorld* World = BoundWorld.Get();
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	Plan->TryGetArrayField(TEXT("projectiles"), Values);
	TSet<FString> CompactIds;
	if (bCompact)
	{
		const TArray<TSharedPtr<FJsonValue>>* Subset = nullptr;
		Plan->TryGetArrayField(TEXT("compact_probe_ids"), Subset);
		if (Subset) for (const TSharedPtr<FJsonValue>& Value : *Subset)
			CompactIds.Add(Value->AsString());
	}
	// Anchored shots fire from PLAYER STARTS (guaranteed playable space):
	// plan-literal AABB-grid origins land inside UE1 solid space where a
	// projectile actor never contacts anything (0/32 hits in the first live
	// run). Anchors stay deterministic: starts are location-sorted.
	TArray<APlayerStart*> AnchorStarts;
	for (TActorIterator<APlayerStart> It(World); It; ++It) AnchorStarts.Add(*It);
	AnchorStarts.Sort([](const APlayerStart& A, const APlayerStart& B)
	{
		const FVector LA = A.GetActorLocation(), LB = B.GetActorLocation();
		return LA.X != LB.X ? LA.X < LB.X : (LA.Y != LB.Y ? LA.Y < LB.Y : LA.Z < LB.Z);
	});
	int32 MaximumTicks = 1;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> Shot = Value->AsObject();
		FString Id, ClassPath; FVector Origin, Direction;
		int32 Lifetime = 300; bool bPortal = false;
		Shot->TryGetStringField(TEXT("id"), Id);
		if (bCompact && !CompactIds.Contains(Id)) continue;
		Shot->TryGetStringField(TEXT("class_path"), ClassPath);
		ReadVector(Shot, TEXT("origin"), Origin); ReadVector(Shot, TEXT("direction"), Direction);
		Shot->TryGetNumberField(TEXT("lifetime_ticks"), Lifetime);
		Shot->TryGetBoolField(TEXT("portal_directed"), bPortal);
		int32 AnchorIndex = -1;
		if (Shot->TryGetNumberField(TEXT("anchor_start_index"), AnchorIndex)
			&& AnchorIndex >= 0 && !AnchorStarts.IsEmpty())
		{
			const APlayerStart* Anchor =
				AnchorStarts[AnchorIndex % AnchorStarts.Num()];
			FString ShotKind;
			Shot->TryGetStringField(TEXT("shot_kind"), ShotKind);
			int32 RadialIndex = 0;
			Shot->TryGetNumberField(TEXT("radial_index"), RadialIndex);
			const double Yaw = 2.0 * PI * (RadialIndex % 8) / 8.0;
			const FVector Radial(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0);
			// Separate same-start shots: a start's floor/wall/oblique trio
			// otherwise spawns overlapping at one point on one tick, and
			// mutual projectile touch/splash races flipped two floor shots'
			// hit/no-hit between the A and B runs.
			Origin = Anchor->GetActorLocation() + FVector(0, 0, 40.0)
				+ Radial * 24.0;
			if (ShotKind == TEXT("floor")) Direction = FVector(0, 0, -1);
			else if (ShotKind == TEXT("oblique"))
				Direction = (Radial + FVector(0, 0, -1)).GetSafeNormal();
			else Direction = Radial;
		}
		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const FRotator ShotRotation = Direction.Rotation();
		AActor* Projectile = Class ? World->SpawnActor<AActor>(
			Class, Origin, ShotRotation, Params) : nullptr;
		if (!Projectile) { Fail(FString::Printf(TEXT("failed projectile %s"), *Id)); return; }
		Projectile->OnActorHit.AddDynamic(this,
			&UWoTBSPProofSubsystem::HandleProjectileActorHit);
		TInlineComponentArray<UProjectileMovementComponent*> Movements;
		Projectile->GetComponents(Movements);
		for (UProjectileMovementComponent* Movement : Movements)
		{
			const float Speed = Movement->InitialSpeed > 0 ? Movement->InitialSpeed : 2000.f;
			Movement->Velocity = Direction.GetSafeNormal() * Speed;
			Movement->Activate(true);
		}
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("id"), Id);
		Item->SetBoolField(TEXT("portal_directed"), bPortal);
		Item->SetBoolField(TEXT("blocking_hit"), false);
		ProjectileRecordIndices.Add(TWeakObjectPtr<AActor>(Projectile),
			ProjectileRecords.Num());
		ProjectileExpiryTicks.Add(TWeakObjectPtr<AActor>(Projectile), Lifetime);
		ProjectileRecords.Add(MakeShared<FJsonValueObject>(Item));
		ActiveProjectiles.Add(Projectile);
		MaximumTicks = FMath::Max(MaximumTicks, Lifetime);
	}
	ProjectileTicksRemaining = MaximumTicks;
	// WoT projectiles explode + tear down inside NotifyHit, before the
	// engine's OnActorHit broadcast can reach us (0/32 recorded hits in the
	// live run) — observe through the projectile's read-only impact probe.
	if (!ImpactProbeHandle.IsValid())
	{
		ImpactProbeHandle = AWoTGenericProjectile::OnImpactProbe.AddUObject(
			this, &UWoTBSPProofSubsystem::RecordProjectileHit);
	}
	State = bCompact ? EState::TravelCompactProjectiles : EState::CollisionProjectiles;
}

void UWoTBSPProofSubsystem::HandleProjectileActorHit(AActor* SelfActor,
	AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit)
{
	(void)NormalImpulse;
	if (SelfActor && SelfActor == SeekingProjectile.Get())
	{
		AActor* HitActor = Hit.GetActor() ? Hit.GetActor() : OtherActor;
		SeekingHitActorClass = HitActor ? HitActor->GetClass()->GetPathName() : FString();
		return;
	}
	RecordProjectileHit(SelfActor, Hit);
}

void UWoTBSPProofSubsystem::HandleSeekingDestroyed(AActor* DestroyedActor)
{
	if (!DestroyedActor) return;
	LastSeekingPosition = DestroyedActor->GetActorLocation();
	SeekingTrajectory.Add(MakeShared<FJsonValueArray>(VectorJson(LastSeekingPosition)));
	if (SeekingSideSign == 0)
	{
		const FVector TrajectoryVector = LastSeekingPosition - SeekingSpawnPosition;
		const double Cross = SeekingObstructionVector.X * TrajectoryVector.Y
			- SeekingObstructionVector.Y * TrajectoryVector.X;
		if (FMath::Abs(Cross) > UE_DOUBLE_KINDA_SMALL_NUMBER)
			SeekingSideSign = Cross > 0.0 ? 1 : -1;
	}
	bSeekingDestroyedAtTarget = FVector::DistSquared(
		LastSeekingPosition, SeekingTargetPosition) <= FMath::Square(25.0);
}

void UWoTBSPProofSubsystem::RecordProjectileHit(AActor* Projectile,
	const FHitResult& Hit)
{
	const int32* Index = ProjectileRecordIndices.Find(
		TWeakObjectPtr<AActor>(Projectile));
	if (!Index || !ProjectileRecords.IsValidIndex(*Index)) return;
	TSharedPtr<FJsonObject> Item = ProjectileRecords[*Index]->AsObject();
	if (!Item.IsValid() || Item->GetBoolField(TEXT("blocking_hit"))) return;
	const FString Id = Item->GetStringField(TEXT("id"));
	const bool bPortal = Item->GetBoolField(TEXT("portal_directed"));
	Item = HitJson(Id, TEXT("projectile"), Projectile->GetActorLocation(),
		Hit.ImpactPoint, Hit);
	Item->SetBoolField(TEXT("portal_directed"), bPortal);
	ProjectileRecords[*Index] = MakeShared<FJsonValueObject>(Item.ToSharedRef());
}

void UWoTBSPProofSubsystem::FinishProjectiles()
{
	if (ImpactProbeHandle.IsValid())
	{
		AWoTGenericProjectile::OnImpactProbe.Remove(ImpactProbeHandle);
		ImpactProbeHandle.Reset();
	}
	for (const TWeakObjectPtr<AActor>& Projectile : ActiveProjectiles)
		if (Projectile.IsValid()) Projectile->Destroy();
	ActiveProjectiles.Reset(); ProjectileRecordIndices.Reset(); ProjectileExpiryTicks.Reset();
	if (bCompactRun)
	{
		BeginNavWait(true);
	}
	else
	{
		WriteCollision(OutDir / TEXT("collision.json"));
		if (Mode == EMode::Collision) FinishSuccess();
		else BeginNavWait(false);
	}
}

void UWoTBSPProofSubsystem::WriteCollision(const FString& Filename)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("probes"), ProbeRecords);
	Root->SetArrayField(TEXT("settles"), SettleRecords);
	Root->SetArrayField(TEXT("projectiles"), ProjectileRecords);
	FString Error; if (!WriteJson(Filename, Root, Error)) Fail(Error);
}

void UWoTBSPProofSubsystem::BeginNavWait(bool bCompact)
{
	bNavCompact = bCompact;
	NavWaitTicks = 0;
	NavQuietTicks = 0;
	State = EState::NavWait;
	UWorld* World = BoundWorld.Get();
	UNavigationSystemV1* Nav =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	UWoTPayloadSubsystem* Payloads = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (!Nav || !Payloads) return;
	// SYMMETRIC full-rebuild epoch on BOTH sides: B needs it because a cached
	// install can complete before BeginPlay (its dirty epoch unobservable);
	// and a lazily-generated reference navmesh sampled without the same epoch
	// under-tiles (packaged A measured 18 tiles vs B's 32, flipping one nav
	// pair's reachability). Dirtying the whole volume makes both sides sample
	// a complete rebuild of identical bounds.
	FBox DirtyBounds(ForceInit);
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
	{
		DirtyBounds += It->GetComponentsBoundingBox(true);
	}
	if (DirtyBounds.IsValid)
	{
		Nav->AddDirtyArea(DirtyBounds, ENavigationDirtyFlag::All,
			TEXT("WoTBSPProofPostInstall"));
		bNavObservedActive = true;
	}
	else
	{
		Fail(TEXT("navigation capture has no NavMeshBoundsVolume"));
	}
}

void UWoTBSPProofSubsystem::StartNav(bool bCompact)
{
	UWorld* World = BoundWorld.Get();
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!Nav) { Fail(TEXT("navigation system is absent")); return; }
	NavPathRecords.Reset(); NavBoundsRecord.Reset(); SeekingRecord.Reset();
	SeekingTrajectory.Reset(); SeekingHitActorClass.Reset(); SeekingSideSign = 0;
	bSeekingDestroyedAtTarget = false;
	NavTileCount = BSPProofActiveNavTiles(World);
	if (TActorIterator<ANavMeshBoundsVolume> It(World); It)
	{
		const FBox Bounds = It->GetComponentsBoundingBox(true);
		NavBoundsRecord = MakeShared<FJsonObject>();
		NavBoundsRecord->SetStringField(TEXT("actor"), It->GetName());
		NavBoundsRecord->SetArrayField(TEXT("box_min"), VectorJson(Bounds.Min));
		NavBoundsRecord->SetArrayField(TEXT("box_max"), VectorJson(Bounds.Max));
	}
	TSet<FString> CompactIds;
	if (bCompact)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		Plan->TryGetArrayField(TEXT("compact_nav_ids"), Values);
		if (Values) for (const TSharedPtr<FJsonValue>& Value : *Values)
			CompactIds.Add(Value->AsString());
	}
	const TArray<TSharedPtr<FJsonValue>>* Pairs = nullptr;
	Plan->TryGetArrayField(TEXT("nav_pairs"), Pairs);
	for (const TSharedPtr<FJsonValue>& Value : *Pairs)
	{
		const TSharedPtr<FJsonObject> Pair = Value->AsObject();
		FString Id; FVector Start, End;
		Pair->TryGetStringField(TEXT("id"), Id);
		if (bCompact && !CompactIds.Contains(Id)) continue;
		ReadVector(Pair, TEXT("start"), Start); ReadVector(Pair, TEXT("end"), End);
		FNavLocation ProjectedStart, ProjectedEnd;
		const bool bStart = Nav->ProjectPointToNavigation(Start, ProjectedStart);
		const bool bEnd = Nav->ProjectPointToNavigation(End, ProjectedEnd);
		UNavigationPath* Path = bStart && bEnd
			? UNavigationSystemV1::FindPathToLocationSynchronously(
				World, ProjectedStart.Location, ProjectedEnd.Location) : nullptr;
		double Length = 0;
		if (Path) for (int32 Index = 1; Index < Path->PathPoints.Num(); ++Index)
			Length += FVector::Distance(Path->PathPoints[Index - 1], Path->PathPoints[Index]);
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("id"), Id);
		Item->SetBoolField(TEXT("found"), Path && Path->IsValid());
		Item->SetBoolField(TEXT("partial"), Path && Path->IsPartial());
		Item->SetArrayField(TEXT("projected_start"), VectorJson(
			bStart ? ProjectedStart.Location : Start));
		Item->SetArrayField(TEXT("projected_end"), VectorJson(
			bEnd ? ProjectedEnd.Location : End));
		Item->SetNumberField(TEXT("length"), Length);
		NavPathRecords.Add(MakeShared<FJsonValueObject>(Item));
	}
	if (NavTileCount <= 0)
	{
		if (Mode == EMode::Travel) WriteTravelCapture();
		else WriteNav(OutDir / TEXT("nav.json"));
		Fail(TEXT("navigation capture has zero active tiles"));
		return;
	}
	const bool bFinalTravelSeeking = bCompact && Mode == EMode::Travel
		&& TravelIndex + 1 >= TravelCount;
	if (bCompact && !bFinalTravelSeeking) { FinishTravelCycle(); return; }
	const TSharedPtr<FJsonObject>* SeekPlan = nullptr;
	Plan->TryGetObjectField(TEXT("seeking"), SeekPlan);
	FString ClassPath; FVector Spawn, Target, ObstructionCenter;
	int32 Lifetime = 600;
	(*SeekPlan)->TryGetStringField(TEXT("class_path"), ClassPath);
	ReadVector(*SeekPlan, TEXT("spawn"), Spawn); ReadVector(*SeekPlan, TEXT("target"), Target);
	ReadVector(*SeekPlan, TEXT("obstruction_center"), ObstructionCenter);
	(*SeekPlan)->TryGetNumberField(TEXT("lifetime_ticks"), Lifetime);
	SeekingRecord = MakeShared<FJsonObject>();
	SeekingRecord->SetStringField(TEXT("id"), TEXT("seeking"));
	FCollisionQueryParams LOSParams(SCENE_QUERY_STAT(WoTBSPProofSeekingLOS), true);
	FHitResult LOSHit;
	const bool bLOSBlocked = World->LineTraceSingleByChannel(
		LOSHit, Spawn, Target, ECC_WorldStatic, LOSParams) && LOSHit.bBlockingHit;
	SeekingRecord->SetBoolField(TEXT("los_blocked"), bLOSBlocked);
	SeekingRecord->SetStringField(TEXT("los_hit_actor_class"), LOSHit.GetActor()
		? LOSHit.GetActor()->GetClass()->GetPathName() : FString());
	SeekingSpawnPosition = Spawn;
	SeekingTargetPosition = Target;
	SeekingObstructionVector = ObstructionCenter - Spawn;
	LastSeekingPosition = Spawn;
	SeekingTrajectory.Add(MakeShared<FJsonValueArray>(VectorJson(Spawn)));
	if (!bLOSBlocked)
	{
		SeekingRecord->SetStringField(TEXT("side"), TEXT("straight"));
		SeekingRecord->SetNumberField(TEXT("side_sign"), 0);
		SeekingRecord->SetStringField(TEXT("completion_reason"), TEXT("destroyed"));
		SeekingRecord->SetStringField(TEXT("preflight_failure"), TEXT("los_clear"));
		SeekingRecord->SetStringField(TEXT("hit_actor_class"), FString());
		SeekingRecord->SetArrayField(TEXT("trajectory"), SeekingTrajectory);
		SeekingRecord->SetArrayField(TEXT("reached_endpoint"), VectorJson(Spawn));
		if (Mode == EMode::Travel) WriteTravelCapture();
		else WriteNav(OutDir / TEXT("nav.json"));
		Fail(TEXT("seeking preflight LOS is clear"));
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
	AActor* TargetActor = World->SpawnActor<AActor>(AActor::StaticClass(),
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (TargetActor)
	{
		USceneComponent* Root = NewObject<USceneComponent>(TargetActor, TEXT("ProofRoot"));
		TargetActor->AddInstanceComponent(Root); TargetActor->SetRootComponent(Root);
		Root->RegisterComponent(); TargetActor->SetActorLocation(Target);
	}
	const FRotator SeekRotation = (Target - Spawn).Rotation();
	AActor* Seeker = Class ? World->SpawnActor<AActor>(
		Class, Spawn, SeekRotation, Params) : nullptr;
	if (!TargetActor || !Seeker)
	{
		if (TargetActor) TargetActor->Destroy();
		Fail(TEXT("failed to spawn seeking projectile case")); return;
	}
	SeekingProjectile = Seeker; SeekingTarget = TargetActor;
	Seeker->OnActorHit.AddDynamic(this,
		&UWoTBSPProofSubsystem::HandleProjectileActorHit);
	Seeker->OnDestroyed.AddDynamic(this,
		&UWoTBSPProofSubsystem::HandleSeekingDestroyed);
	if (AWoTSeekingProjectile* Typed = Cast<AWoTSeekingProjectile>(Seeker))
		Typed->SetDestination(TargetActor);
	else { Seeker->Destroy(); TargetActor->Destroy();
		Fail(TEXT("seeking class is not AWoTSeekingProjectile")); return; }
	NavSeekingTicksRemaining = Lifetime; State = EState::NavSeeking;
}

void UWoTBSPProofSubsystem::FinishNavSeeking(const FString& CompletionReason)
{
	if (SeekingProjectile.IsValid())
	{
		LastSeekingPosition = SeekingProjectile->GetActorLocation();
	}
	SeekingRecord->SetStringField(TEXT("side"), SeekingSideSign > 0
		? TEXT("left") : (SeekingSideSign < 0 ? TEXT("right") : TEXT("straight")));
	SeekingRecord->SetNumberField(TEXT("side_sign"), SeekingSideSign);
	SeekingRecord->SetStringField(TEXT("completion_reason"), CompletionReason);
	SeekingRecord->SetStringField(TEXT("hit_actor_class"), SeekingHitActorClass);
	SeekingRecord->SetArrayField(TEXT("trajectory"), SeekingTrajectory);
	SeekingRecord->SetArrayField(TEXT("reached_endpoint"),
		VectorJson(LastSeekingPosition));
	if (SeekingProjectile.IsValid())
	{
		SeekingProjectile->OnDestroyed.RemoveDynamic(
			this, &UWoTBSPProofSubsystem::HandleSeekingDestroyed);
		SeekingProjectile->Destroy();
	}
	if (SeekingTarget.IsValid()) SeekingTarget->Destroy();
	SeekingProjectile.Reset(); SeekingTarget.Reset();
	if (Mode == EMode::Travel) FinishTravelCycle();
	else
	{
		WriteNav(OutDir / TEXT("nav.json"));
		FinishSuccess();
	}
}

void UWoTBSPProofSubsystem::WriteNav(const FString& Filename)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("bounds"), NavBoundsRecord.IsValid()
		? NavBoundsRecord.ToSharedRef() : MakeShared<FJsonObject>());
	Root->SetNumberField(TEXT("tile_count"), NavTileCount);
	Root->SetNumberField(TEXT("wait_ticks"), NavWaitTicks);
	Root->SetArrayField(TEXT("paths"), NavPathRecords);
	Root->SetObjectField(TEXT("seeking"), SeekingRecord.IsValid()
		? SeekingRecord.ToSharedRef() : MakeShared<FJsonObject>());
	FString Error; if (!WriteJson(Filename, Root, Error)) Fail(Error);
}

void UWoTBSPProofSubsystem::BeginTravel()
{
	bReturningToArena = false;
	State = EState::TravelToFrontEnd;
	UGameplayStatics::OpenLevel(BoundWorld.Get(), FName(*FrontEndPackage), true);
}

bool UWoTBSPProofSubsystem::WriteTravelCapture()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("cycle"), TravelIndex);
	Root->SetNumberField(TEXT("used_physical"),
		static_cast<double>(FPlatformMemory::GetStats().UsedPhysical));
	Root->SetArrayField(TEXT("probes"), ProbeRecords);
	Root->SetArrayField(TEXT("projectiles"), ProjectileRecords);
	Root->SetArrayField(TEXT("nav_paths"), NavPathRecords);
	Root->SetNumberField(TEXT("nav_tiles"), NavTileCount);
	Root->SetNumberField(TEXT("nav_wait_ticks"), NavWaitTicks);
	if (SeekingRecord.IsValid()) Root->SetObjectField(
		TEXT("seeking"), SeekingRecord.ToSharedRef());
	FString Error;
	if (!WriteJson(OutDir / FString::Printf(TEXT("travel_%d.json"), TravelIndex),
		Root, Error))
	{
		Fail(Error);
		return false;
	}
	return true;
}

void UWoTBSPProofSubsystem::FinishTravelCycle()
{
	if (!WriteTravelCapture()) return;
	++TravelIndex;
	if (TravelIndex >= TravelCount) { FinishSuccess(); return; }
	bCompactRun = false; bReturningToArena = false; State = EState::TravelToFrontEnd;
	UGameplayStatics::OpenLevel(BoundWorld.Get(), FName(*FrontEndPackage), true);
}

void UWoTBSPProofSubsystem::WriteProofLog()
{
	if (!bActive || !LogDevice || OutDir.IsEmpty()) return;
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("records"), LogDevice->Snapshot());
	FString Error; WriteJson(OutDir / TEXT("proof_log.json"), Root, Error);
}

void UWoTBSPProofSubsystem::FinishSuccess()
{
	if (State == EState::Finished || State == EState::Failed) return;
	State = EState::Finished;
	const FWoTBSPProofCounters Counters = UWoTPayloadSubsystem::GetBSPProofCounters();
	TSharedRef<FJsonObject> CounterJson = MakeShared<FJsonObject>();
	CounterJson->SetNumberField(TEXT("render_builds"), Counters.RenderBuilds);
	CounterJson->SetNumberField(TEXT("collision_cooks"), Counters.CollisionCooks);
	CounterJson->SetNumberField(TEXT("derived_loads"), Counters.DerivedLoads);
	CounterJson->SetNumberField(TEXT("derived_writes"), Counters.DerivedWrites);
	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetStringField(TEXT("status"), TEXT("ok"));
	Summary->SetStringField(TEXT("mode"), ModeName(Mode));
	Summary->SetNumberField(TEXT("travels"), TravelIndex);
	Summary->SetObjectField(TEXT("counters"), CounterJson);
	if (UWoTPayloadSubsystem* Payloads = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr)
	{
		Summary->SetBoolField(TEXT("carrier_seen"), Payloads->HasSeenBSPCarrier());
		Summary->SetStringField(TEXT("cache_directory"), Payloads->GetCacheDirectory());
	}
	FString Error; WriteJson(OutDir / TEXT("summary.json"), Summary, Error);
	WriteProofLog();
	FPlatformMisc::RequestExit(false);
}

void UWoTBSPProofSubsystem::Fail(const FString& Reason)
{
	if (State == EState::Failed) return;
	FailureReason = Reason.IsEmpty() ? TEXT("unspecified proof failure") : Reason;
	// Always name the reason in the log: a failure before the output
	// directory exists (or whose write fails) is otherwise completely silent
	// in a packaged client.
	UE_LOG(LogTemp, Error, TEXT("WOT-BSP-PROOF-FAIL: %s (out=%s)"),
		*FailureReason, *OutDir);
	State = EState::Failed;
	if (!OutDir.IsEmpty() && !FPaths::IsRelative(OutDir))
	{
		IFileManager::Get().MakeDirectory(*OutDir, true);
		const FWoTBSPProofCounters Counters = UWoTPayloadSubsystem::GetBSPProofCounters();
		TSharedRef<FJsonObject> CounterJson = MakeShared<FJsonObject>();
		CounterJson->SetNumberField(TEXT("render_builds"), Counters.RenderBuilds);
		CounterJson->SetNumberField(TEXT("collision_cooks"), Counters.CollisionCooks);
		CounterJson->SetNumberField(TEXT("derived_loads"), Counters.DerivedLoads);
		CounterJson->SetNumberField(TEXT("derived_writes"), Counters.DerivedWrites);
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("status"), TEXT("failed"));
		Summary->SetStringField(TEXT("error"), FailureReason);
		Summary->SetObjectField(TEXT("counters"), CounterJson);
		FString Error; WriteJson(OutDir / TEXT("summary.json"), Summary, Error);
		WriteProofLog();
	}
	FPlatformMisc::RequestExitWithStatus(false, 1);
}

#else

UWoTBSPProofSubsystem::UWoTBSPProofSubsystem(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UWoTBSPProofSubsystem::~UWoTBSPProofSubsystem() = default;

bool UWoTBSPProofSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	(void)Outer;
	return false;
}

void UWoTBSPProofSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UWoTBSPProofSubsystem::Deinitialize()
{
	Super::Deinitialize();
}

bool UWoTBSPProofSubsystem::ValidatePlanJson(const FString& Text, FString& OutError)
{
	(void)Text;
	OutError = TEXT("BSP proof harness is disabled in this target");
	return false;
}

int32 UWoTBSPProofSubsystem::ResolveFaceSection(const UStaticMesh* Mesh, int32 FaceIndex)
{
	(void)Mesh;
	(void)FaceIndex;
	return INDEX_NONE;
}

bool UWoTBSPProofSubsystem::BuildRenderMeshJson(
	UStaticMesh* Mesh, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	(void)Mesh;
	OutJson.Reset();
	OutError = TEXT("BSP proof harness is disabled in this target");
	return false;
}

// The delegate targets are declared unconditionally (UHT forbids UFUNCTION
// inside preprocessor blocks); without the harness they are inert stubs.
void UWoTBSPProofSubsystem::HandleProjectileActorHit(AActor*, AActor*,
	FVector, const FHitResult&)
{
}

void UWoTBSPProofSubsystem::HandleSeekingDestroyed(AActor*)
{
}

#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
