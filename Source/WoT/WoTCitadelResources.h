#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
// IsValidDescriptor's `Adapter != nullptr` instantiates
// TSubclassOf<AWoTInventory>::operator UClass*, which needs the COMPLETE type —
// a forward declaration compiles only while every including TU happens to see
// the real header first (unity-chunk luck). Include it here so the header is
// self-sufficient.
#include "Inventory/WoTInventory.h"

class AActor;

struct FWoTResourceDescriptor
{
	TSubclassOf<class AWoTInventory> Adapter;
	UClass* ResourceClass = nullptr;
	FName CreatureRow = NAME_None;
	FName BaseResourceType = NAME_None;
	bool bSpawnTempResource = false;

	// SealInventory has a resource actor class but neither a creature row nor a trap
	// class; the registered adapter CDO remains the descriptor's canonical identity.
	bool IsValidDescriptor() const { return Adapter != nullptr; }
};

enum class EWoTCitadelSelectableState : uint8
{
	None,
	Inactive,
	Active,
};

enum class EWoTCitadelSelectableRejection : uint8
{
	None,
	InvalidCandidate,
	InvalidSession,
	UnknownSelection,
	MissingLogicalResource,
	WrongResourceClass,
	CreateResource,
	MissingEditZone,
	ForeignEditZone,
};

struct FWoTCitadelSelectableResource
{
	AActor* LogicalResource = nullptr;
	FName AdapterName = NAME_None;
	const FWoTResourceDescriptor* Descriptor = nullptr;
	EWoTCitadelSelectableState State = EWoTCitadelSelectableState::None;
	EWoTCitadelSelectableRejection Rejection =
		EWoTCitadelSelectableRejection::InvalidCandidate;

	bool IsSelectable() const
	{
		return Rejection == EWoTCitadelSelectableRejection::None;
	}
};

enum class EWoTCitadelEditorAction : uint8
{
	None,
	Create,
	Remove,
	Toggle,
	Deactivate,
};

// Value-only cursor hit returned by AWoTPlayerController::EditorTrace. A trap child is
// already replaced by its logical resource, while location/normal remain the physical
// surface impact used by placement.
struct FWoTCitadelEditorTraceResult
{
	AActor* HitActor = nullptr;
	FVector HitLocation = FVector::ZeroVector;
	FVector HitNormal = FVector::ZeroVector;
	bool bBlockingHit = false;
	bool bHitWorldGeometry = false;
};

// Side-effect-free snapshot of EditorHUD's selected WOTInventory fields.
struct FWoTCitadelEditorSelection
{
	UClass* ResourceClass = nullptr;
	int32 Count = 0;
	bool bSpawnTempResource = false;

	bool IsSelected() const { return ResourceClass != nullptr; }
};

namespace WoTCitadelResources
{
	// BudgetInfo adapter name -> descriptor. Unregistered adapters return an empty
	// descriptor. This replaces UE1's DynamicLoadObject for trap, troop, and seal cards.
	const FWoTResourceDescriptor& FindByAdapterName(FName AdapterName);

	// Selected resource actor class -> its budget adapter name (NAME_None if unregistered).
	FName FindAdapterForResourceClass(const UClass* ResourceClass);
	// Folded creature-row identity -> its pawn adapter (NAME_None if unregistered).
	FName FindAdapterForCreatureRow(FName CreatureRow);
	// BudgetInfo.GetResourceType's CDO BaseResourceType comparison, factored so the
	// authority budget and owner-only mirror use exactly the same tier predicate.
	bool IsAdapterForTier(FName AdapterName, FName TierKeyword);

	// Side-effect-free multi-actor identity seam used by selection, traces, and specs.
	AActor* ResolveLogicalResource(AActor* Candidate);

	// Canonical selectable query. Resolves the selected descriptor and logical base, then
	// requires the matching trap class or deployed creature row and the session team's edit
	// zone. Budget count and viewer-zone action authority are deliberately absent.
	FWoTCitadelSelectableResource QuerySelectableResource(
		AActor* Candidate, FName SelectedAdapterName, uint8 SessionTeam);

	// Pure port of EditorHUD.LeftMouseDown's world/resource decision tree. Authority,
	// budget mutation, input, and presentation are deliberately outside this helper.
	EWoTCitadelEditorAction ResolveEditorAction(
		const FWoTCitadelEditorTraceResult& Hit,
		const FWoTCitadelEditorSelection& Selection);
}
