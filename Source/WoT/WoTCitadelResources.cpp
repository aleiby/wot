#include "WoTCitadelResources.h"

#include "AI/WoTNPCCharacter.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTSeal.h"
#include "Inventory/WoTTroopInventories.h"
#include "Traps/WoTAlarm.h"
#include "Traps/WoTPortcullis.h"
#include "Traps/WoTPit.h"
#include "Traps/WoTSpear.h"
#include "Traps/WoTStaircase.h"
#include "Traps/WoTWall.h"
#include "Traps/WoTFireWall.h"
#include "WoTBattleGameMode.h"
#include "WoTZoneVolume.h"

namespace
{
	FWoTResourceDescriptor MakeCitadelResourceDescriptor(
		TSubclassOf<AWoTInventory> Adapter)
	{
		FWoTResourceDescriptor Descriptor;
		Descriptor.Adapter = Adapter;
		if (const AWoTInventory* CDO =
			Adapter ? Adapter->GetDefaultObject<AWoTInventory>() : nullptr)
		{
			Descriptor.ResourceClass = CDO->ResourceClass.Get();
			Descriptor.CreatureRow = CDO->CreatureRowName;
			Descriptor.BaseResourceType = CDO->BaseResourceType;
			Descriptor.bSpawnTempResource = CDO->SpawnTempResource;
		}
		return Descriptor;
	}

	const TMap<FName, FWoTResourceDescriptor>& CitadelResourceTable()
	{
		static const TMap<FName, FWoTResourceDescriptor> Table = {
			{TEXT("SpearInventory"),
			 MakeCitadelResourceDescriptor(AWoTSpearInventory::StaticClass())},
			{TEXT("AlarmInventory"),
			 MakeCitadelResourceDescriptor(AWoTAlarmInventory::StaticClass())},
			{TEXT("PortcullisInventory"),
			 MakeCitadelResourceDescriptor(AWoTPortcullisInventory::StaticClass())},
			{TEXT("StaircaseInventory"),
			 MakeCitadelResourceDescriptor(AWoTStaircaseInventory::StaticClass())},
			{TEXT("WallInventory"),
			 MakeCitadelResourceDescriptor(AWoTWallInventory::StaticClass())},
			{TEXT("FireWallInventory"),
			 MakeCitadelResourceDescriptor(AWoTFireWallInventory::StaticClass())},
			{TEXT("PitInventory"),
			 MakeCitadelResourceDescriptor(AWoTPitInventory::StaticClass())},
			{TEXT("WarderInventory"),
			 MakeCitadelResourceDescriptor(AWoTWarderInventory::StaticClass())},
			{TEXT("SisterInventory"),
			 MakeCitadelResourceDescriptor(AWoTSisterInventory::StaticClass())},
			{TEXT("SitterInventory"),
			 MakeCitadelResourceDescriptor(AWoTSitterInventory::StaticClass())},
			{TEXT("TrollocInventory"),
			 MakeCitadelResourceDescriptor(AWoTTrollocInventory::StaticClass())},
			{TEXT("MyrddraalInventory"),
			 MakeCitadelResourceDescriptor(AWoTMyrddraalInventory::StaticClass())},
			{TEXT("BATrollocInventory"),
			 MakeCitadelResourceDescriptor(AWoTBATrollocInventory::StaticClass())},
			{TEXT("MinionInventory"),
			 MakeCitadelResourceDescriptor(AWoTMinionInventory::StaticClass())},
			{TEXT("MashadarInventory"),
			 MakeCitadelResourceDescriptor(AWoTMashadarInventory::StaticClass())},
			{TEXT("LegionInventory"),
			 MakeCitadelResourceDescriptor(AWoTLegionInventory::StaticClass())},
			{TEXT("SoldierInventory"),
			 MakeCitadelResourceDescriptor(AWoTSoldierInventory::StaticClass())},
			{TEXT("ArcherInventory"),
			 MakeCitadelResourceDescriptor(AWoTArcherInventory::StaticClass())},
			{TEXT("QuestionerInventory"),
			 MakeCitadelResourceDescriptor(AWoTQuestionerInventory::StaticClass())},
			{TEXT("SealInventory"),
			 MakeCitadelResourceDescriptor(AWoTSealInventory::StaticClass())},
		};
		return Table;
	}
}

const FWoTResourceDescriptor& WoTCitadelResources::FindByAdapterName(FName AdapterName)
{
	static const FWoTResourceDescriptor Empty;
	const FWoTResourceDescriptor* Found = CitadelResourceTable().Find(AdapterName);
	return Found ? *Found : Empty;
}

FName WoTCitadelResources::FindAdapterForResourceClass(const UClass* ResourceClass)
{
	if (!ResourceClass)
	{
		return NAME_None;
	}
	for (const TPair<FName, FWoTResourceDescriptor>& Entry : CitadelResourceTable())
	{
		// All pawn adapters fold to AWoTNPCCharacter; their row is the disambiguator.
		if (Entry.Value.CreatureRow.IsNone()
			&& Entry.Value.ResourceClass == ResourceClass)
		{
			return Entry.Key;
		}
	}
	return NAME_None;
}

FName WoTCitadelResources::FindAdapterForCreatureRow(FName CreatureRow)
{
	if (CreatureRow.IsNone())
	{
		return NAME_None;
	}
	for (const TPair<FName, FWoTResourceDescriptor>& Entry : CitadelResourceTable())
	{
		if (Entry.Value.CreatureRow == CreatureRow)
		{
			return Entry.Key;
		}
	}
	return NAME_None;
}

bool WoTCitadelResources::IsAdapterForTier(FName AdapterName, FName TierKeyword)
{
	if (AdapterName.IsNone() || TierKeyword.IsNone())
	{
		return false;
	}
	const FWoTResourceDescriptor& Descriptor = FindByAdapterName(AdapterName);
	return Descriptor.IsValidDescriptor()
		&& Descriptor.BaseResourceType == TierKeyword;
}

AActor* WoTCitadelResources::ResolveLogicalResource(AActor* Candidate)
{
	AWoTTrap* CandidateTrap = Cast<AWoTTrap>(Candidate);
	if (IsValid(CandidateTrap))
	{
		return CandidateTrap->GetBaseResource();
	}
	AWoTNPCCharacter* CandidateTroop = Cast<AWoTNPCCharacter>(Candidate);
	AWoTBattleGameMode* BattleMode = IsValid(CandidateTroop) && CandidateTroop->GetWorld()
		? CandidateTroop->GetWorld()->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (IsValid(BattleMode))
	{
		return BattleMode->IsCitadelDeployedTroop(CandidateTroop)
			? CandidateTroop : nullptr;
	}
	// GameMode is authority-only. A remote owner may optimistically identify a
	// registered creature row for cursor/remove dispatch; ServerRemoveResource repeats
	// the authoritative weak-registry check, so a summon can never be destroyed/refunded.
	return IsValid(CandidateTroop) && CandidateTroop->GetWorld()
		&& CandidateTroop->GetWorld()->GetNetMode() == NM_Client
		&& !FindAdapterForCreatureRow(CandidateTroop->CreatureRowName).IsNone()
		? CandidateTroop : nullptr;
}

FWoTCitadelSelectableResource WoTCitadelResources::QuerySelectableResource(
	AActor* Candidate, FName SelectedAdapterName, uint8 SessionTeam)
{
	FWoTCitadelSelectableResource Result;
	if (!IsValid(Candidate))
	{
		return Result;
	}
	if (SessionTeam == 255)
	{
		Result.Rejection = EWoTCitadelSelectableRejection::InvalidSession;
		return Result;
	}

	const FWoTResourceDescriptor& SelectedDescriptor =
		FindByAdapterName(SelectedAdapterName);
	if (!SelectedDescriptor.IsValidDescriptor())
	{
		Result.Rejection = EWoTCitadelSelectableRejection::UnknownSelection;
		return Result;
	}
	Result.AdapterName = SelectedAdapterName;
	Result.Descriptor = &SelectedDescriptor;

	Result.LogicalResource = ResolveLogicalResource(Candidate);
	AWoTTrap* LogicalTrap = Cast<AWoTTrap>(Result.LogicalResource);
	AWoTNPCCharacter* LogicalTroop = Cast<AWoTNPCCharacter>(Result.LogicalResource);
	if (!IsValid(LogicalTrap) && !IsValid(LogicalTroop))
	{
		Result.Rejection = EWoTCitadelSelectableRejection::MissingLogicalResource;
		return Result;
	}
	const FName LogicalAdapter = IsValid(LogicalTroop)
		? FindAdapterForCreatureRow(LogicalTroop->CreatureRowName)
		: FindAdapterForResourceClass(LogicalTrap->GetClass());
	if (LogicalAdapter != SelectedAdapterName)
	{
		Result.Rejection = EWoTCitadelSelectableRejection::WrongResourceClass;
		return Result;
	}
	if (IsValid(LogicalTrap) && SelectedDescriptor.bSpawnTempResource)
	{
		Result.Rejection = EWoTCitadelSelectableRejection::CreateResource;
		return Result;
	}

	AWoTZoneVolume* EditZone = AWoTZoneVolume::EditZoneAt(
		Result.LogicalResource->GetWorld(), Result.LogicalResource->GetActorLocation());
	if (!IsValid(EditZone))
	{
		Result.Rejection = EWoTCitadelSelectableRejection::MissingEditZone;
		return Result;
	}
	if (EditZone->EditTeam != SessionTeam)
	{
		Result.Rejection = EWoTCitadelSelectableRejection::ForeignEditZone;
		return Result;
	}

	Result.State = IsValid(LogicalTroop) || LogicalTrap->IsTrapDeployed()
		? EWoTCitadelSelectableState::Active : EWoTCitadelSelectableState::Inactive;
	Result.Rejection = EWoTCitadelSelectableRejection::None;
	return Result;
}

EWoTCitadelEditorAction WoTCitadelResources::ResolveEditorAction(
	const FWoTCitadelEditorTraceResult& Hit,
	const FWoTCitadelEditorSelection& Selection)
{
	if (!Hit.bBlockingHit)
	{
		return EWoTCitadelEditorAction::None;
	}

	AActor* LogicalResource = ResolveLogicalResource(Hit.HitActor);
	AWoTTrap* Resource = Cast<AWoTTrap>(LogicalResource);
	AWoTNPCCharacter* Troop = Cast<AWoTNPCCharacter>(LogicalResource);
	if (Hit.bHitWorldGeometry && !IsValid(Resource) && !IsValid(Troop))
	{
		return Selection.IsSelected() && Selection.bSpawnTempResource
			&& Selection.Count > 0
			? EWoTCitadelEditorAction::Create
			: EWoTCitadelEditorAction::None;
	}
	if (IsValid(Troop))
	{
		return EWoTCitadelEditorAction::Remove;
	}
	if (!IsValid(Resource))
	{
		return EWoTCitadelEditorAction::None;
	}

	const bool bToggleResource = Resource->IsA<AWoTPortcullis>()
		|| Resource->IsA<AWoTStaircase>();
	if (!bToggleResource)
	{
		return EWoTCitadelEditorAction::Remove;
	}
	// EditorHUD.LeftMouseDown: a Portcullis/Staircase toggles only when its class matches
	// the selection AND count>0; EVERY other case (wrong class, no selection, zero count)
	// falls through to Deactivate (EditorHUD.uc:282-292) — not None.
	if (Selection.IsSelected()
		&& Resource->GetClass() == Selection.ResourceClass && Selection.Count > 0)
	{
		return EWoTCitadelEditorAction::Toggle;
	}
	return EWoTCitadelEditorAction::Deactivate;
}
