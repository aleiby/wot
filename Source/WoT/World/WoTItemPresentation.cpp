#include "WoTItemPresentation.h"

#include "WoTRuntimeActorVisuals.h"
#include "WoTRuntimeClassDefaults.h"
#include "../Inventory/WoTInventory.h"
#include "../WoTMeshLight.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTVertexAnimComponent.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "../RetailData/WoTDevRetailFamilies.h"
#endif
#include "Components/SceneComponent.h"
#include "Engine/World.h"

AWoTItemPresentation::AWoTItemPresentation()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetReplicateMovement(false);
	SetActorEnableCollision(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->SetHiddenInSceneCapture(true);
	Mesh->PrimaryComponentTick.bStartWithTickEnabled = false;
}

AWoTItemPresentation* AWoTItemPresentation::Spawn(UWorld* World,
	FName ClassName, const FTransform& Transform)
{
	if (!IsValid(World)) return nullptr;
	const AWoTInventory* Default = AWoTInventory::FindDefaultByOriginalName(ClassName);
	if (!Default) return nullptr;
	FString Error;
	const FWoTRuntimeClassDefaults* Defaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
#if !WOT_GUARDED_BOOTSTRAP
	if (!Defaults) Defaults = WoTDevRetailClassDefaults();
#endif
	if (!Defaults) return nullptr;
	FString NativeLeaf = Default->GetClass()->GetName();
	if (NativeLeaf.StartsWith(TEXT("WoT"), ESearchCase::CaseSensitive))
		NativeLeaf.RightChopInline(3);
	FString ClassIdentity;
	if (!Defaults->FindClassIdentityByLeaf(NativeLeaf, ClassIdentity)) return nullptr;
	FWoTRuntimeActorVisualPresentation Presentation;
	if (!WoTRuntimeReadClassVisualPresentation(
		*Defaults, ClassIdentity, Presentation, Error)) return nullptr;
	return SpawnFromPresentation(World, Presentation, Transform);
}

AWoTItemPresentation* AWoTItemPresentation::SpawnFromPresentation(UWorld* World,
	const FWoTRuntimeActorVisualPresentation& Presentation,
	const FTransform& Transform)
{
	if (!IsValid(World) || !Presentation.HasMesh()) return nullptr;
	AWoTItemPresentation* Actor = World->SpawnActorDeferred<AWoTItemPresentation>(
		StaticClass(), Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Actor) return nullptr;
	Actor->ScaleGlow = Presentation.ScaleGlow;
	Actor->AmbientGlow = Presentation.AmbientGlow;
	Actor->Style = Presentation.Style;
	Actor->bUnlit = Presentation.bUnlit;
	Actor->bStyleTranslucent = Presentation.bStyleTranslucent;
	FString Error;
	if (!WoTRuntimeApplyActorVisualPresentation(*Actor->Mesh, Presentation, Error))
	{
		Actor->Destroy();
		return nullptr;
	}
	// Deferred construction keeps the first component BeginPlay lighting bake
	// behind the final owner properties, transform and mesh presentation.
	Actor->FinishSpawning(Transform);
	Actor->Mesh->SetComponentTickEnabled(false);
	Actor->Mesh->EnsureRuntimeRenderState();
	UWoTMeshLight::AttachRuntime(Actor);
	return Actor;
}

FBox AWoTItemPresentation::CalculateComponentsBoundingBoxInLocalSpace(
	bool bNonColliding, bool bIncludeFromChildActors) const
{
	// The mesh's own bounds are a culling volume, and an imported asset and a
	// runtime mesh answer them differently. Callers of this size the visual, so
	// answer with what the shown pose draws.
	FBox DrawnBox(ForceInit);
	if (Mesh && Mesh->IsRegistered()
		&& (bNonColliding || Mesh->IsCollisionEnabled())
		&& Mesh->GetCurrentPoseDrawnBox(DrawnBox))
	{
		return DrawnBox.TransformBy(Mesh->GetComponentTransform()
			.GetRelativeTransform(GetActorTransform()));
	}
	return Super::CalculateComponentsBoundingBoxInLocalSpace(
		bNonColliding, bIncludeFromChildActors);
}
