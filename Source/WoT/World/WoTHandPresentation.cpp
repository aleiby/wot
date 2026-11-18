#include "WoTHandPresentation.h"

#include "../WoTFactionCharacters.h"
#include "../WoTMeshLight.h"
#include "../WoTPlayerState.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
// PlayerReplicationInfo.Color order (WOTPlayer.uc SetSkin): 0 base, 1 black,
// 2 blue, 3 red, 4 green.
const TCHAR* const HandColours[] = {
	TEXT("base"), TEXT("black"), TEXT("blue"), TEXT("red"), TEXT("green")};
}

AWoTHandPresentation::AWoTHandPresentation()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetReplicateMovement(false);
	SetActorEnableCollision(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Hand = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("Hand"));
	Hand->SetupAttachment(RootComponent);
	Hand->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Hand->SetGenerateOverlapEvents(false);
	Hand->SetCastShadow(false);
	Hand->SetHiddenInSceneCapture(true);
}

bool AWoTHandPresentation::StyleForPawn(const APawn* Pawn, FName& OutHandClass,
	int32& OutColor)
{
	if (!IsValid(Pawn)) return false;
	if (Pawn->IsA<AWoTAesSedai>()) OutHandClass = TEXT("channeler");
	else if (Pawn->IsA<AWoTForsaken>()) OutHandClass = TEXT("darklord");
	else if (Pawn->IsA<AWoTHound>()) OutHandClass = TEXT("hound");
	else if (Pawn->IsA<AWoTWhitecloak>()) OutHandClass = TEXT("zealot");
	else return false;
	const AWoTPlayerState* State = Pawn->GetPlayerState<AWoTPlayerState>();
	OutColor = State ? FMath::Clamp<int32>(State->Color, 0, 4) : 0;
	return true;
}

AWoTHandPresentation* AWoTHandPresentation::Spawn(UWorld* World, FName HandClass,
	int32 Color, const FTransform& Transform)
{
#if WOT_GUARDED_BOOTSTRAP
	// Guarded builds cook no hand art yet; callers fall back to the item alone.
	return nullptr;
#else
	if (!IsValid(World) || HandClass.IsNone()) return nullptr;
	const FString Key = HandClass.ToString();
	const FString Root = FString::Printf(TEXT("/Game/WoT/FirstPerson/Hands/%s"), *Key);
	const TCHAR* const Colour = HandColours[FMath::Clamp(Color, 0, 4)];
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,
		*FString::Printf(TEXT("%s/%s/SkeletalMeshes/%s.%s"), *Root, *Key, *Key, *Key),
		nullptr, LOAD_NoWarn | LOAD_Quiet);
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr,
		*FString::Printf(TEXT("%s/T_%s_%s.T_%s_%s"), *Root, *Key, Colour, *Key, Colour),
		nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshLit.M_WoT_MeshLit"),
		nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Mesh || !Texture || !Master) return nullptr;

	AWoTHandPresentation* Actor = World->SpawnActorDeferred<AWoTHandPresentation>(
		StaticClass(), Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Actor) return nullptr;
	Actor->Hand->SetSkinnedAssetAndUpdate(Mesh);
	for (int32 Slot = 0; Slot < Actor->Hand->GetNumMaterials(); ++Slot)
	{
		// The world's lit mesh master; the mesh-light baker keeps a MID of a
		// target master and only adds the actor's lighting parameters.
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Master, Actor);
		Mid->SetTextureParameterValue(TEXT("Tex"), Texture);
		Mid->SetVectorParameterValue(TEXT("KeyColor"), FLinearColor::Black);
		Actor->Hand->SetMaterial(Slot, Mid);
	}
	Actor->FinishSpawning(Transform);
	UWoTMeshLight::AttachRuntime(Actor);
	return Actor;
#endif
}
