#include "WoTWorldProjector.h"

#include "WoTRuntimeClassDefaults.h"
#include "WoTRuntimeActorVisuals.h"
#include "WoTRuntimeBSPRenderer.h"
#include "WoTRuntimeParticleProjection.h"
#include "WoTRuntimeWorldProperties.h"
#include "WoTWorldMaterializer.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Assets/WoTRetailIdentity.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTLegionInvSeeker.h"
#include "../Inventory/WoTAndilayRoot.h"
#include "../Inventory/WoTInventory.h"
#include "../Inventory/WoTZoneLeeches.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTBounceableDecoration.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTAmbientSound.h"
#include "../WoTActorRotator.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTDecoration.h"
#include "../WoTDecal.h"
#include "../WoTDynamicLightSubsystem.h"
#include "../WoTDestroyDirective.h"
#include "../WoTExplodingWall.h"
#include "../WoTCorona.h"
#include "../WoTFogLight.h"
#include "../WoTLightWave.h"
#include "../WoTLeafMap.h"
#include "../WoTDoorMover.h"
#include "../WoTDoorTrigger.h"
#include "../WoTExplodingMover.h"
#include "../WoTAttachMover.h"
#include "../WoTAssertMover.h"
#include "../WoTElevatorMover.h"
#include "../WoTGradualMover.h"
#include "../WoTJumper.h"
#include "../WoTLoopMover.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTMixMover.h"
#include "../WoTMover.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTPlayerStart.h"
#include "../WoTRotatingMover.h"
#include "../WoTRainCyd.h"
#include "../WoTSealAltar.h"
#include "../WoTSoundDispatcher.h"
#include "../WoTSpecialTriggers.h"
#include "../WoTTeleporter.h"
#include "../WoTTimedMover.h"
#include "../WoTFactionCharacters.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTStaircase.h"
#include "../WoTZoneVolume.h"
#include "../WoTWorldFX.h"

#include "Components/BoxComponent.h"
#include "Components/BrushComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/SkyLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/PointLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "Misc/Paths.h"

namespace
{
const FName PlayerStartRule(TEXT("PlayerStart"));
const FName NPCRule(TEXT("NPC"));
const FName SpawnPoolRule(TEXT("SpawnPool"));
const FName ZoneRule(TEXT("Zone"));
const FName LightRule(TEXT("Light"));
const FName SkyLightRule(TEXT("SkyLight"));
const FName NavBoundsRule(TEXT("NavBounds"));
const FName PathNodeRule(TEXT("PathNode"));
const FName ParticleSprayerRule(TEXT("ParticleSprayer"));
const FName FixedPickupRule(TEXT("FixedPickup"));
const FName AndilayRootRule(TEXT("AndilayRoot"));
const FName DecorationRule(TEXT("Decoration"));
const FName BreakableDecorationRule(TEXT("BreakableDecoration"));
const FName BounceableDecorationRule(TEXT("BounceableDecoration"));
const FName DecalManagerRule(TEXT("DecalManager"));
const FName ExplodingWallRule(TEXT("ExplodingWall"));
const FName DestroyDirectiveRule(TEXT("DestroyDirective"));
const FName LightTableRule(TEXT("LightTable"));
const FName MoverRule(TEXT("Mover"));
const FName TriggerRule(TEXT("Trigger"));
const FName DoorTriggerRule(TEXT("DoorTrigger"));
const FName DispatcherRule(TEXT("Dispatcher"));
const FName CounterRule(TEXT("Counter"));
const FName SoundDispatcherRule(TEXT("SoundDispatcher"));
const FName TaggedMarkerRule(TEXT("TaggedMarker"));
const FName PortcullisRule(TEXT("Portcullis"));
const FName PitOpeningRule(TEXT("PitOpening"));
const FName StaircaseRule(TEXT("Staircase"));
const FName TeleporterRule(TEXT("Teleporter"));
const FName JumperRule(TEXT("Jumper"));
const FName BlockAllRule(TEXT("BlockAll"));
const FName BlockPlayerRule(TEXT("BlockPlayer"));
const FName BlockMonstersRule(TEXT("BlockMonsters"));
const FName SpecialEventTriggerRule(TEXT("SpecialEventTrigger"));
const FName SpecialEventRule(TEXT("SpecialEvent"));
const FName SpecialEffectsTriggerRule(TEXT("SpecialEffectsTrigger"));
const FName StochasticTriggerRule(TEXT("StochasticTrigger"));
const FName TriggerLightRule(TEXT("TriggerLight"));
const FName BattleInfoRule(TEXT("BattleInfo"));
const FName SealAltarRule(TEXT("SealAltar"));
const FName BudgetRule(TEXT("Budget"));
const FName MashadarGuideRule(TEXT("MashadarGuide"));
const FName AmbientSoundRule(TEXT("AmbientSound"));
const FName DynamicAmbientSoundRule(TEXT("DynamicAmbientSound"));
const FName WaterSoundZoneRule(TEXT("WaterSoundZone"));
const FName CoronaRule(TEXT("Corona"));
const FName LeafMapRule(TEXT("LeafMap"));
const FName ActorRotatorRule(TEXT("ActorRotator"));
const FName RainCydRule(TEXT("RainCyd"));
const FName FogLightRule(TEXT("FogLight"));
const FName LightWaveRule(TEXT("LightWave"));
const FName LightningRule(TEXT("Lightning"));
const FName RandomLightningRule(TEXT("RandomLightning"));
const FName DistanceLightningRule(TEXT("DistanceLightning"));
const FName WindRule(TEXT("Wind"));
const FName SpewTriggerRule(TEXT("SpewTrigger"));

template <typename T>
T* SpawnDeferred(UWorld& World, int32 RecordIndex, FName Rule,
	const FTransform& Transform, int32 Suffix = INDEX_NONE)
{
	const FString Text = Suffix == INDEX_NONE
		? FString::Printf(TEXT("WoTProjected_%d_%s"), RecordIndex, *Rule.ToString())
		: FString::Printf(TEXT("WoTProjected_%d_%s_%d"), RecordIndex,
			*Rule.ToString(), Suffix);
	FActorSpawnParameters P;
	P.OverrideLevel = World.PersistentLevel;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.bDeferConstruction = true;
	P.ObjectFlags |= RF_Transient;
	P.Name = FName(*Text);
	return World.SpawnActor<T>(T::StaticClass(), Transform, P);
}

template <typename T>
T* SpawnFallback(UWorld& World, const TCHAR* Name, const FTransform& Transform)
{
	FActorSpawnParameters P;
	P.OverrideLevel = World.PersistentLevel;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.bDeferConstruction = true;
	P.ObjectFlags |= RF_Transient;
	P.Name = FName(Name);
	return World.SpawnActor<T>(T::StaticClass(), Transform, P);
}

AActor* SpawnDeferredClass(UWorld& World, UClass& ActorClass,
	int32 RecordIndex, FName Rule, const FTransform& Transform)
{
	const FString Text = FString::Printf(
		TEXT("WoTProjected_%d_%s"), RecordIndex, *Rule.ToString());
	FActorSpawnParameters P;
	P.OverrideLevel = World.PersistentLevel;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.bDeferConstruction = true;
	P.ObjectFlags |= RF_Transient;
	P.Name = FName(*Text);
	return World.SpawnActor(&ActorClass, &Transform, P);
}

void Count(FWoTWorldProjectionResult& Out, FName Rule)
{
	Out.RuleCounts.FindOrAdd(Rule)++;
}

void SkipActor(FWoTWorldProjectionResult& Out, FName Rule,
	FStringView Record, FStringView Reason)
{
	Out.SkippedRuleCounts.FindOrAdd(Rule)++;
	// Keep the complete release-census sample set.  The largest production map
	// currently has eleven reviewed skips, and truncating at eight made it
	// impossible for the release audit to name every source record.
	if (Out.SkippedSamples.Num() >= 64) return;
	FString SafeRecord(Record.Left(96));
	FString SafeReason(Reason.Left(160));
	SafeRecord.ReplaceInline(TEXT("\r"), TEXT(" "));
	SafeRecord.ReplaceInline(TEXT("\n"), TEXT(" "));
	SafeReason.ReplaceInline(TEXT("\r"), TEXT(" "));
	SafeReason.ReplaceInline(TEXT("\n"), TEXT(" "));
	SafeRecord.ReplaceInline(TEXT(" "), TEXT("%20"));
	SafeReason.ReplaceInline(TEXT(" "), TEXT("%20"));
	Out.SkippedSamples.Add(SafeRecord + TEXT(":") + SafeReason);
}

FString Leaf(FStringView Identity)
{
	FString Result(Identity);
	int32 Dot = INDEX_NONE;
	if (Result.FindLastChar(TEXT('.'), Dot)) Result.RightChopInline(Dot + 1);
	int32 Export = INDEX_NONE;
	if (Result.FindLastChar(TEXT('#'), Export)
		&& FStringView(Result).Mid(Export).StartsWith(
			TEXT("#export-"), ESearchCase::IgnoreCase))
	{
		const FStringView Digits = FStringView(Result).Mid(Export + 8);
		bool bNumeric = !Digits.IsEmpty();
		for (const TCHAR Character : Digits)
			bNumeric = bNumeric && FChar::IsDigit(Character);
		if (bNumeric) Result.LeftInline(Export);
	}
	return Result;
}

bool FitsProjectedName(FStringView Value)
{
	// FName includes the terminator in NAME_SIZE and asserts before substituting
	// an error name when an added value is 1024 characters or longer.
	return Value.Len() < NAME_SIZE;
}

bool IsExact(FStringView A, FStringView B)
{
	return A.Equals(B, ESearchCase::IgnoreCase);
}

const FWoTRuntimeWorldProperty* Prop(const FWoTRuntimeClassDefaults& Defaults,
	const FWoTRuntimeWorldActor& Actor, const TCHAR* Name, uint32 Slot = 0)
{
	return Defaults.FindProperty(Actor, Name, Slot);
}

bool ReadByte(const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	const TCHAR* Name, uint8& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	return P == nullptr || WoTRuntimeWorldPropertyByte(*P, Value);
}

bool ReadInt(const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	const TCHAR* Name, int32& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr) return true;
	if (WoTRuntimeWorldPropertyInteger(*P, Value)) return true;
	uint8 Byte = 0;
	if (!WoTRuntimeWorldPropertyByte(*P, Byte)) return false;
	Value = Byte; return true;
}

bool ReadFloat(const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	const TCHAR* Name, float& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr) return true;
	if (WoTRuntimeWorldPropertyFloat(*P, Value)) return true;
	int32 Integer = 0;
	if (!WoTRuntimeWorldPropertyInteger(*P, Integer)) return false;
	Value = static_cast<float>(Integer); return true;
}

bool ReadBool(const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	const TCHAR* Name, bool& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	return P == nullptr || WoTRuntimeWorldPropertyBool(*P, Value);
}

bool ReadVector(const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	const TCHAR* Name, FVector& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	return P == nullptr || WoTRuntimeWorldPropertyVector(*P, Value);
}

bool ReadRotator(const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	const TCHAR* Name, FRotator& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	return P == nullptr || WoTRuntimeWorldPropertyRotator(*P, Value);
}

bool ReadName(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, FName& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr) return true;
	if (P->Type != EWoTRuntimeWorldPropertyType::Name
		|| !FitsProjectedName(P->NameValue)) return false;
	Value = FName(*P->NameValue);
	return true;
}

/** "/Game/WoT/<Sounds|Textures>/" sub path of a retail object reference:
 * "Pkg/Name", or the exact "Pkg/Group/.../Name" when the name collides inside
 * its package and the authenticated registry holds that exact identity
 * (WoTRetailIdentity.h; e.g. AmbientA.Nature vs .Bush.Rustle3). */
bool RetailObjectSubPath(FStringView ObjectIdentity, EWoTNeutralResourceKind Kind,
	FString& Out)
{
	FWoTRetailObjectPath Path;
	if (!WoTParseRetailObjectPath(ObjectIdentity, Path)) return false;
	Path.Name = Leaf(Path.Name);
	Out = Kind == EWoTNeutralResourceKind::Texture
		? WoTRetailTextureSubPath(Path) : WoTRetailAssetSubPath(Kind, Path);
	return true;
}

bool ReadObjectSubPath(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, FName& Value,
	EWoTNeutralResourceKind Kind = EWoTNeutralResourceKind::Sound)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr) return true;
	if (P->Type != EWoTRuntimeWorldPropertyType::Object) return false;
	if (P->ObjectIdentity.IsEmpty()) return true;
	FString SubPath;
	if (!RetailObjectSubPath(P->ObjectIdentity, Kind, SubPath)) return false;
	if (!FitsProjectedName(SubPath)) return false;
	Value = FName(*SubPath);
	return true;
}

bool ActorTransform(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, FTransform& Out)
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	if (!ReadVector(D, A, TEXT("Location"), Location)
		|| !ReadRotator(D, A, TEXT("Rotation"), Rotation)) return false;
	Out = FTransform(Rotation, Location); return true;
}

bool ReadString(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, FString& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr) return true;
	// UE1 serializes an empty FString as CompactIndex(0) only. Localized class
	// defaults can resolve to that value when the localized package has no row;
	// preserve the native constructor's reviewed fallback text in that case.
	if (P->Type == EWoTRuntimeWorldPropertyType::String
		&& P->Value.Num() == 1 && P->Value[0] == 0) return true;
	return WoTRuntimeWorldPropertyUE1String(*P, Value);
}

FString CanonicalObjectIdentity(FStringView Identity)
{
	FString Result(Identity);
	int32 Dot = INDEX_NONE;
	if (!Result.FindLastChar(TEXT('.'), Dot)) return Leaf(Identity);
	return Result.Left(Dot + 1) + Leaf(Result.Mid(Dot + 1));
}

bool ReadObjectIdentity(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, FName& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr || P->ObjectIdentity.IsEmpty()) return true;
	if (P->Type != EWoTRuntimeWorldPropertyType::Object) return false;
	const FString Identity = CanonicalObjectIdentity(P->ObjectIdentity);
	if (!FitsProjectedName(Identity)) return false;
	Value = FName(*Identity);
	return true;
}

bool ReadObjectLeafName(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, FName& Value)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr || P->ObjectIdentity.IsEmpty()) return true;
	if (P->Type != EWoTRuntimeWorldPropertyType::Object) return false;
	const FString NameLeaf = Leaf(P->ObjectIdentity);
	if (!FitsProjectedName(NameLeaf)) return false;
	Value = FName(*NameLeaf);
	return true;
}

bool ReadFloatSlots(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, int32 Count,
	TArray<float>& Out, bool bAlways)
{
	bool bAny = false;
	TArray<float> Values;
	Values.SetNumZeroed(Count);
	for (int32 Slot = 0; Slot < Count; ++Slot)
	{
		const FWoTRuntimeWorldProperty* P = Prop(D, A, Name, Slot);
		if (!P) continue;
		bAny = true;
		if (!WoTRuntimeWorldPropertyFloat(*P, Values[Slot])) return false;
	}
	if (bAlways || bAny) Out = MoveTemp(Values);
	return true;
}

bool ReadNameSlots(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, int32 Count,
	TArray<FName>& Out, bool bAlways, bool bSounds = false)
{
	bool bAny = false;
	TArray<FName> Values;
	Values.SetNum(Count);
	for (int32 Slot = 0; Slot < Count; ++Slot)
	{
		const FWoTRuntimeWorldProperty* P = Prop(D, A, Name, Slot);
		if (!P) continue;
		bAny = true;
		if (bSounds)
		{
			if (P->Type != EWoTRuntimeWorldPropertyType::Object) return false;
			if (!P->ObjectIdentity.IsEmpty())
			{
				FString Path;
				if (!RetailObjectSubPath(P->ObjectIdentity,
					EWoTNeutralResourceKind::Sound, Path)) return false;
				if (!FitsProjectedName(Path)) return false;
				Values[Slot] = FName(*Path);
			}
		}
		else
		{
			if (P->Type != EWoTRuntimeWorldPropertyType::Name
				|| !FitsProjectedName(P->NameValue)) return false;
			Values[Slot] = FName(*P->NameValue);
		}
	}
	if (bAlways || bAny) Out = MoveTemp(Values);
	return true;
}

bool ReadObjectLeafSlots(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, int32 Count,
	TArray<FName>& Out)
{
	Out.SetNum(Count);
	for (int32 Slot = 0; Slot < Count; ++Slot)
	{
		const FWoTRuntimeWorldProperty* P = Prop(D, A, Name, Slot);
		if (!P) continue;
		if (P->Type != EWoTRuntimeWorldPropertyType::Object) return false;
		if (P->ObjectIdentity.IsEmpty()) continue;
		const FString Identity = CanonicalObjectIdentity(P->ObjectIdentity);
		if (!FitsProjectedName(Identity)) return false;
		Out[Slot] = FName(*Identity);
	}
	return true;
}

bool ReadMoverTransform(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, FTransform& Out)
{
	FVector BasePosition = FVector::ZeroVector;
	FRotator BaseRotation = FRotator::ZeroRotator;
	if (!ReadVector(D, A, TEXT("Location"), BasePosition)) return false;
	if (!ReadVector(D, A, TEXT("BasePos"), BasePosition)
		|| !ReadRotator(D, A, TEXT("BaseRot"), BaseRotation)) return false;
	Out = FTransform(BaseRotation, BasePosition);
	return true;
}

// Engine.Scale is serialized as Scale (3 floats), SheerRate (float) and
// SheerAxis (byte): 17 bytes. No shipped MP mover shears (census of all 270);
// a non-zero SheerRate is rejected rather than silently dropped.
bool ReadMoverScale(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const TCHAR* Name, FVector& Out)
{
	const FWoTRuntimeWorldProperty* P = Prop(D, A, Name);
	if (P == nullptr) return true;
	if (P->Type != EWoTRuntimeWorldPropertyType::Struct
		|| !P->StructName.Equals(TEXT("Scale"), ESearchCase::IgnoreCase)
		|| P->Value.Num() != 17) return false;
	float Values[4];
	FMemory::Memcpy(Values, P->Value.GetData(), sizeof(Values));
	for (const float Value : Values)
		if (!FMath::IsFinite(Value)) return false;
	if (Values[3] != 0.0f) return false;
	Out = FVector(Values[0], Values[1], Values[2]);
	return true;
}

bool ReadMoverBrushFrame(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, const FTransform& Transform,
	FWoTMoverBrushFrame& Out)
{
	Out.InitialRotation = Transform.Rotator();
	return ReadVector(D, A, TEXT("PrePivot"), Out.PrePivot)
		&& ReadMoverScale(D, A, TEXT("MainScale"), Out.MainScale)
		&& ReadMoverScale(D, A, TEXT("PostScale"), Out.PostScale);
}

EWoTMoverState MoverStateFromName(FName State)
{
	const FString Text = State.ToString();
	if (Text.Equals(TEXT("BumpOpenTimed"), ESearchCase::IgnoreCase)) return EWoTMoverState::BumpOpenTimed;
	if (Text.Equals(TEXT("TriggerOpenTimed"), ESearchCase::IgnoreCase)) return EWoTMoverState::TriggerOpenTimed;
	if (Text.Equals(TEXT("TriggerToggle"), ESearchCase::IgnoreCase)) return EWoTMoverState::TriggerToggle;
	if (Text.Equals(TEXT("TriggerControl"), ESearchCase::IgnoreCase)) return EWoTMoverState::TriggerControl;
	if (Text.Equals(TEXT("StandOpenTimed"), ESearchCase::IgnoreCase)) return EWoTMoverState::StandOpenTimed;
	if (Text.Equals(TEXT("TriggerPound"), ESearchCase::IgnoreCase)) return EWoTMoverState::TriggerPound;
	if (Text.Equals(TEXT("BumpButton"), ESearchCase::IgnoreCase)) return EWoTMoverState::BumpButton;
	if (Text.Equals(TEXT("GradualTriggerOpenTimed"), ESearchCase::IgnoreCase)) return EWoTMoverState::GradualTriggerOpenTimed;
	if (Text.Equals(TEXT("GradualTriggerPound"), ESearchCase::IgnoreCase)) return EWoTMoverState::GradualTriggerPound;
	if (Text.Equals(TEXT("GradualTriggerToggle"), ESearchCase::IgnoreCase)) return EWoTMoverState::GradualTriggerToggle;
	return EWoTMoverState::None;
}

bool IsPathClass(FStringView Class)
{
	return IsExact(Class, TEXT("Engine.PathNode"))
		|| IsExact(Class, TEXT("WOT.WotPathNode"))
		|| IsExact(Class, TEXT("WOTPawns.MashadarPathNode"))
		|| IsExact(Class, TEXT("Engine.InventorySpot"))
		|| IsExact(Class, TEXT("Engine.PlayerStart"))
		|| IsExact(Class, TEXT("Engine.Teleporter"))
		|| IsExact(Class, TEXT("Legend.TeleporterWithEffects"));
}

FVector3d UE1FGetHSVDouble(uint8 Hue, uint8 Saturation, uint8 Brightness)
{
	double Channels[3] = {};
	if (Hue < 86)
	{
		Channels[0] = (85.0 - Hue) / 85.0;
		Channels[1] = static_cast<double>(Hue) / 85.0;
	}
	else if (Hue < 171)
	{
		Channels[1] = (170.0 - Hue) / 85.0;
		Channels[2] = (static_cast<double>(Hue) - 85.0) / 85.0;
	}
	else
	{
		Channels[0] = (static_cast<double>(Hue) - 170.0) / 85.0;
		Channels[2] = (255.0 - Hue) / 84.0;
	}
	double Value = static_cast<double>(Brightness) * 1.4 / 255.0;
	Value *= 0.70 / (0.01 + FMath::Sqrt(Value));
	Value = FMath::Clamp(Value, 0.0, 1.0);
	const double White = static_cast<double>(Saturation) / 255.0;
	return FVector3d(
		(Channels[0] + White * (1.0 - Channels[0])) * Value,
		(Channels[1] + White * (1.0 - Channels[1])) * Value,
		(Channels[2] + White * (1.0 - Channels[2])) * Value);
}

FLinearColor UE1FGetHSV(uint8 Hue, uint8 Saturation, uint8 Brightness)
{
	const FVector3d Color=UE1FGetHSVDouble(Hue,Saturation,Brightness);
	return FLinearColor(
		static_cast<float>(Color.X),static_cast<float>(Color.Y),
		static_cast<float>(Color.Z),1.0f);
}

struct FRuntimeLightDefaults
{
	uint8 Type = 0;
	uint8 Effect = 0;
	uint8 Brightness = 0;
	uint8 Hue = 0;
	uint8 Saturation = 0;
	uint8 Radius = 0;
	uint8 Cone = 0;
	bool bSunLightRadius = false;
};

// Keep this table in lockstep with the native composer's
// BSPWorld.cpp:WoTWorldLightDefaults. Engine script is stripped in retail, so
// the decoded class-default graph alone is not an authority for these fields.
bool RuntimeLightDefaults(FStringView LeafClass, FRuntimeLightDefaults& Out)
{
	Out = FRuntimeLightDefaults();
	if (LeafClass.Equals(TEXT("Light"), ESearchCase::IgnoreCase)
		|| LeafClass.Equals(TEXT("TriggerLight"), ESearchCase::IgnoreCase)
		|| LeafClass.Equals(TEXT("Spotlight"), ESearchCase::IgnoreCase))
	{
		Out.Type = 1; Out.Brightness = 64; Out.Saturation = 255;
		Out.Radius = 64; Out.Cone = 128;
		if (LeafClass.Equals(TEXT("Spotlight"), ESearchCase::IgnoreCase))
			Out.Effect = 12;
	}
	else if (LeafClass.Equals(TEXT("SunLight"), ESearchCase::IgnoreCase))
	{
		Out.Type = 1; Out.Brightness = 255; Out.Saturation = 255;
		Out.Radius = 64; Out.Cone = 128; Out.bSunLightRadius = true;
	}
	else if (LeafClass.Equals(TEXT("PixieSprayerBlue"), ESearchCase::IgnoreCase))
	{
		Out.Type = 1; Out.Effect = 13; Out.Brightness = 100;
		Out.Hue = 140; Out.Saturation = 64; Out.Radius = 25;
	}
	else if (!LeafClass.Equals(TEXT("ParticleSprayer"), ESearchCase::IgnoreCase)
		&& !LeafClass.Equals(TEXT("Flame01"), ESearchCase::IgnoreCase)
		&& !LeafClass.Equals(TEXT("LampFlame01"), ESearchCase::IgnoreCase)
		&& !LeafClass.Equals(TEXT("PitOpening"), ESearchCase::IgnoreCase)
		&& !LeafClass.Equals(TEXT("Cylinder"), ESearchCase::IgnoreCase)
		&& !LeafClass.Equals(TEXT("twotriangles"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	return true;
}

bool ReadInstanceByte(const FWoTRuntimeWorldActor& Actor,
	const TCHAR* Name, uint8& Value)
{
	const FWoTRuntimeWorldProperty* Property =
		WoTRuntimeWorldFindProperty(Actor, Name);
	return Property == nullptr || WoTRuntimeWorldPropertyByte(*Property, Value);
}

bool ReadInstanceVector(const FWoTRuntimeWorldActor& Actor,
	const TCHAR* Name, FVector& Value)
{
	const FWoTRuntimeWorldProperty* Property =
		WoTRuntimeWorldFindProperty(Actor, Name);
	return Property == nullptr || WoTRuntimeWorldPropertyVector(*Property, Value);
}

bool ReadInstanceRotatorUnits(const FWoTRuntimeWorldActor& Actor,
	const TCHAR* Name, FIntVector& Value)
{
	const FWoTRuntimeWorldProperty* Property =
		WoTRuntimeWorldFindProperty(Actor, Name);
	if (Property == nullptr) return true;
	if (Property->Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property->StructName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase)
		|| Property->Value.Num() != 12) return false;
	int32 Values[3];
	FMemory::Memcpy(Values, Property->Value.GetData(), sizeof(Values));
	Value = FIntVector(Values[0], Values[1], Values[2]);
	return true;
}

bool ReadInstanceName(const FWoTRuntimeWorldActor& Actor,
	const TCHAR* Name, FName& Value)
{
	const FWoTRuntimeWorldProperty* Property =
		WoTRuntimeWorldFindProperty(Actor, Name);
	if (Property == nullptr) return true;
	if (Property->Type != EWoTRuntimeWorldPropertyType::Name
		|| !FitsProjectedName(Property->NameValue)) return false;
	Value = FName(*Property->NameValue);
	return true;
}

FVector RuntimeLightDirection(const FIntVector& Rotation)
{
	auto SinTable = [](int64 Angle)
	{
		const uint32 Index = static_cast<uint32>(
			(static_cast<uint64>(Angle) >> 2) & 16383ull);
		return FMath::Sin(
			static_cast<double>(Index) * (2.0 * UE_PI / 16384.0));
	};
	const double CosPitch = SinTable(static_cast<int64>(Rotation.X) + 16384);
	const double SinPitch = SinTable(Rotation.X);
	const double CosYaw = SinTable(static_cast<int64>(Rotation.Y) + 16384);
	const double SinYaw = SinTable(Rotation.Y);
	return FVector(CosPitch * CosYaw, CosPitch * SinYaw, SinPitch);
}

double RuntimeProjectedLocation(double Value)
{
	const double Scaled = Value * 1000000.0;
	const double Rounded = Scaled >= 0.0
		? FMath::FloorToDouble(Scaled + 0.5)
		: FMath::CeilToDouble(Scaled - 0.5);
	return Rounded / 1000000.0;
}

FColor ImporterPointLightColor(uint8 Hue, uint8 Saturation)
{
	const float H = Hue / 255.f * 6.f;
	const int32 Sector = FMath::FloorToInt(H);
	const float Fraction = H - Sector;
	const float P = Saturation / 255.f;
	const float S = 1.f - P;
	const float Q = 1.f - S * Fraction;
	const float T = 1.f - S * (1.f - Fraction);
	float R = 1.f, G = T, B = P;
	switch (Sector % 6)
	{
	case 1: R = Q; G = 1.f; B = P; break;
	case 2: R = P; G = 1.f; B = T; break;
	case 3: R = P; G = Q; B = 1.f; break;
	case 4: R = T; G = P; B = 1.f; break;
	case 5: R = 1.f; G = P; B = Q; break;
	default: break;
	}
	return FColor(static_cast<uint8>(FMath::Clamp(R * 255.f, 0.f, 255.f)),
		static_cast<uint8>(FMath::Clamp(G * 255.f, 0.f, 255.f)),
		static_cast<uint8>(FMath::Clamp(B * 255.f, 0.f, 255.f)), 255);
}

bool IsInertClass(const FWoTRuntimeClassDefaults& D, FStringView Class)
{
	static const TCHAR* Bases[] = {
		TEXT("Engine.Info"), TEXT("Engine.Keypoint"), TEXT("Engine.Brush"),
		TEXT("Engine.Projector")
	};
	for (const TCHAR* Base : Bases) if (D.ClassIsA(Class, Base)) return true;
	return IsExact(Class, TEXT("Engine.LevelInfo"))
		|| IsExact(Class, TEXT("Engine.Camera"))
		|| Leaf(Class).Equals(TEXT("FlushTrigger"),ESearchCase::IgnoreCase);
}

void AddInert(FWoTWorldProjectionResult& Out, FStringView Class)
{
	++Out.InertRecordCount;
	Out.InertClassCounts.FindOrAdd(FName(Leaf(Class)))++;
}

void AddUnsupported(FWoTWorldProjectionResult& Out, FStringView Class)
{
	++Out.UnsupportedRecordCount;
	Out.UnsupportedClassCounts.FindOrAdd(FName(Leaf(Class)))++;
	const FString Name(Class);
	if (Out.UnsupportedClassNames.Num() < 32
		&& !Out.UnsupportedClassNames.Contains(Name)) Out.UnsupportedClassNames.Add(Name);
}

bool NodeBounds(const FWoTWorldProjectionSource& Source, int32 Zone,
	FVector& OutMin, FVector& OutMax)
{
	bool Any = false;
	OutMin = FVector(DBL_MAX); OutMax = FVector(-DBL_MAX);
	for (const FWoTRuntimeWorldNode& Node : Source.Nodes)
	{
		if (Node.Zones[0] != Zone && Node.Zones[1] != Zone) continue;
		if (Node.VertexPool < 0 || Node.VertexCount == 0
			|| Node.VertexPool + Node.VertexCount > Source.Vertices.Num()) continue;
		for (int32 I = 0; I < Node.VertexCount; ++I)
		{
			const int32 Point = Source.Vertices[Node.VertexPool + I].Point;
			if (!Source.Points.IsValidIndex(Point)) continue;
			const FVector P(Source.Points[Point]);
			OutMin = OutMin.ComponentMin(P); OutMax = OutMax.ComponentMax(P); Any = true;
		}
	}
	return Any;
}

int32 FindRecord(TConstArrayView<FWoTRuntimeWorldActor> Records, FStringView Identity)
{
	for (int32 I = 0; I < Records.Num(); ++I)
		if (FStringView(Records[I].Identity).Equals(Identity, ESearchCase::IgnoreCase)) return I;
	return INDEX_NONE;
}

bool ProjectPlayerStart(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform T;
	uint8 Team = 0;
	if (!ActorTransform(D, A, T) || !ReadByte(D, A, TEXT("TeamNumber"), Team))
	{ Error = TEXT("a projected PlayerStart has malformed properties"); return false; }
	AWoTPlayerStart* Start = SpawnDeferred<AWoTPlayerStart>(World, Index, PlayerStartRule, T);
	if (!Start) { Error = TEXT("a PlayerStart projection could not be allocated"); return false; }
	Start->SetReplicates(false); Start->TeamNumber = Team;
	Out.DeferredActors.Add(Start); ++Out.PlayerStartCount; Count(Out, PlayerStartRule);
	return true;
}

bool ProjectNPC(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform Transform;
	FName InitThreatGoalEvent;
	FName TakeDamageEvent;
	bool bTakeDamageEventOnce = false;
	if (!ActorTransform(D, A, Transform)
		|| !ReadName(D, A, TEXT("InitThreatGoalEvent"), InitThreatGoalEvent)
		|| !ReadName(D, A, TEXT("TakeDamageEvent"), TakeDamageEvent)
		|| !ReadBool(D, A, TEXT("bTakeDamageEventOnce"),
			bTakeDamageEventOnce))
	{
		Error = TEXT("a projected NPC has malformed authored event properties");
		return false;
	}

	AWoTNPCCharacter* NPC = SpawnDeferred<AWoTNPCCharacter>(
		World, Index, NPCRule, Transform);
	if (!NPC)
	{
		Error = TEXT("an NPC projection could not be allocated");
		return false;
	}

	TArray<FString> ClassChain;
	if (D.GetClassChain(A.ClassIdentity, ClassChain))
	{
		for (const FString& ClassIdentity : ClassChain)
		{
			if (const FWoTCreatureRow* Row = WoTCreatureData::Find(
				FName(*Leaf(ClassIdentity))))
			{
				Row->ConfigureNPC(*NPC);
				break;
			}
		}
	}
	NPC->InitThreatGoalEvent = InitThreatGoalEvent;
	NPC->TakeDamageEvent = TakeDamageEvent;
	NPC->bTakeDamageEventOnce = bTakeDamageEventOnce;
	Out.DeferredActors.Add(NPC);
	Count(Out, NPCRule);
	return true;
}

bool ProjectPool(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform T;
	bool bRespawn = true;
	float RespawnTime = 0.0f;
	if (!ActorTransform(D, A, T)
		|| !ReadBool(D, A, TEXT("bRespawn"), bRespawn)
		|| !ReadFloat(D, A, TEXT("RespawnTime"), RespawnTime))
	{ Error = TEXT("a spawn pool has malformed properties"); return false; }
	TArray<TSubclassOf<AWoTInventory>> Classes;
	TArray<int32> Weights;
	for (uint32 Slot = 0; Slot < 16; ++Slot)
	{
		const FWoTRuntimeWorldProperty* Pool = Prop(D, A, TEXT("Pool"), Slot);
		const FWoTRuntimeWorldProperty* Weight = Prop(D, A, TEXT("Weight"), Slot);
		uint8 SerializedWeight = 0;
		if (Pool && Pool->Type != EWoTRuntimeWorldPropertyType::Object)
		{ Error = TEXT("a spawn pool entry is malformed"); return false; }
		if (Weight && !WoTRuntimeWorldPropertyByte(*Weight, SerializedWeight))
		{ Error = TEXT("a spawn pool weight is malformed"); return false; }
		const int32 W = SerializedWeight;
		// AngrealSpawnPool.GetWeightedSelection stops its distribution at the
		// first None class. A zero-weight class is still an occupied slot.
		if (!Pool || Pool->ObjectIdentity.IsEmpty()) break;
		const FString Native = TEXT("/Script/WoT.WoT") + Leaf(Pool->ObjectIdentity);
		if (!FitsProjectedName(Native))
		{ Error = TEXT("a spawn pool object path is malformed"); return false; }
		UClass* C = FindObject<UClass>(nullptr, *Native);
		if (!C || !C->IsChildOf(AWoTInventory::StaticClass())) { ++Out.DroppedPoolEntries; continue; }
		Classes.Add(C); Weights.Add(W);
	}
	if (Classes.IsEmpty()) { ++Out.EmptyPoolCount; return true; }
	AWoTAngrealSpawnPool* Pool = SpawnDeferred<AWoTAngrealSpawnPool>(World, Index, SpawnPoolRule, T);
	if (!Pool) { Error = TEXT("a spawn pool projection could not be allocated"); return false; }
	Pool->SetReplicates(true); Pool->SetNetAddressable();
	Pool->Pool = MoveTemp(Classes); Pool->Weight = MoveTemp(Weights);
	Pool->bRespawn = bRespawn;
	Pool->RespawnTime = RespawnTime;
	Out.DeferredActors.Add(Pool); Count(Out, SpawnPoolRule); return true;
}

UClass* ResolveNativePickupClass(FStringView RetailClassIdentity)
{
	if (Leaf(RetailClassIdentity).Equals(TEXT("DartForsaken"),
		ESearchCase::IgnoreCase)) return AWoTAngrealInvDart::StaticClass();
	const FString Native = TEXT("/Script/WoT.WoT") + Leaf(RetailClassIdentity);
	if (!FitsProjectedName(Native)) return nullptr;
	UClass* Class = FindObject<UClass>(nullptr, *Native);
	return Class != nullptr && Class->IsChildOf(AWoTInventory::StaticClass())
		&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated
			| CLASS_NewerVersionExists)
		? Class : nullptr;
}

bool HasEffectiveMesh(const FWoTRuntimeClassDefaults& Defaults,
	const FWoTRuntimeWorldActor& Actor)
{
	const FWoTRuntimeWorldProperty* Mesh =
		Defaults.FindProperty(Actor, TEXT("Mesh"));
	return Mesh != nullptr
		&& Mesh->Type == EWoTRuntimeWorldPropertyType::Object
		&& !Mesh->ObjectIdentity.IsEmpty();
}

bool ProjectPickup(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	UClass* Class = ResolveNativePickupClass(A.ClassIdentity);
	if (Class == nullptr)
	{
		Error = TEXT("a fixed pickup has no concrete native gameplay class");
		return false;
	}
	FTransform Transform;
	FWoTRuntimeActorVisualPresentation Visual;
	const bool bDartForsaken = Leaf(A.ClassIdentity).Equals(
		TEXT("DartForsaken"), ESearchCase::IgnoreCase);
	const bool bMeshlessLegion = Class->IsChildOf(AWoTLegionInvSeeker::StaticClass());
	if (!ActorTransform(D, A, Transform)
		|| !(bDartForsaken
			? WoTRuntimeReadClassVisualPresentation(
				D, TEXT("Angreal.AngrealInvDart"), Visual, Error)
			: WoTRuntimeReadActorVisualPresentation(D, A, Visual, Error))
		|| (!Visual.HasMesh() && !bMeshlessLegion))
	{
		if (Error.IsEmpty()) Error = TEXT("a fixed pickup has no effective mesh");
		return false;
	}
	const bool bRoot = Class->IsChildOf(AWoTAndilayRoot::StaticClass());
	const FName Rule = bRoot ? AndilayRootRule : FixedPickupRule;
	AWoTInventory* Pickup = Cast<AWoTInventory>(
		SpawnDeferredClass(World, *Class, Index, Rule, Transform));
	if (!Pickup)
	{
		Error = TEXT("a fixed pickup projection could not be allocated"); return false;
	}
	Pickup->SetReplicates(true);
	Pickup->SetNetAddressable();
	if (Visual.HasMesh()) Pickup->ConfigureRuntimeVisualPresentation(Visual);
	if (bDartForsaken)
	{
		// DartForsaken.PreBeginPlay replaces itself with a base dart, calls
		// SetColor('Red'), copies only the four charge fields below, and destroys
		// the black wrapper. Presentation therefore comes from AngrealInvDart.
		CastChecked<AWoTAngrealInvDart>(Pickup)->SetColor(TEXT("Red"));
	}
	float CollisionRadius = Pickup->Collision->GetUnscaledSphereRadius();
	FName Tag;
	if (!ReadFloat(D, A, TEXT("RespawnTime"), Pickup->RespawnTime)
		|| !ReadFloat(D, A, TEXT("CollisionRadius"), CollisionRadius)
		|| !ReadName(D, A, TEXT("Tag"), Tag) || CollisionRadius <= 0.0f)
	{
		Error = TEXT("a fixed pickup property is malformed");
		Pickup->Destroy(); return false;
	}
	Pickup->Collision->SetSphereRadius(CollisionRadius, false);
	if (!Tag.IsNone()) Pickup->Tags.AddUnique(Tag);
	if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Pickup))
	{
		if (!ReadInt(D, A, TEXT("MinInitialCharges"), Angreal->MinInitialCharges)
			|| !ReadInt(D, A, TEXT("MaxInitialCharges"), Angreal->MaxInitialCharges)
			|| !ReadInt(D, A, TEXT("MaxCharges"), Angreal->MaxCharges)
			|| !ReadInt(D, A, TEXT("ChargeCost"), Angreal->ChargeCost)
			|| !ReadBool(D, A, TEXT("bMPAppearEffect"), Angreal->bMPAppearEffect))
		{
			Error = TEXT("a fixed angreal property is malformed");
			Pickup->Destroy(); return false;
		}
		for (uint32 Slot = 0; Slot < 3; ++Slot)
		{
			const FWoTRuntimeWorldProperty* Property =
				Prop(D, A, TEXT("SPInitialCharges"), Slot);
			if (Property && !WoTRuntimeWorldPropertyInteger(
				*Property, Angreal->SPInitialCharges[Slot]))
			{
				Error = TEXT("a fixed angreal SPInitialCharges value is malformed");
				Pickup->Destroy(); return false;
			}
		}
	}
	Out.DeferredActors.Add(Pickup);
	Count(Out, Rule);
	return true;
}

bool WoTProjectDecorationSpriteSeries(const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& Defaults,
	const FWoTRuntimeWorldActor& Actor, TArray<FString>& OutIdentities,
	float& OutWidth, float& OutHeight, float& OutMinRate, float& OutMaxRate,
	FString& Error)
{
	const TFunction<bool(FStringView, FWoTRetailTextureFacts&)>& FindTexture =
		Source.FindDecorationTexture
		? Source.FindDecorationTexture : Source.FindTexture;
	const FWoTRuntimeWorldProperty* Texture =
		Prop(Defaults, Actor, TEXT("Texture"));
	if (Texture == nullptr
		|| Texture->Type != EWoTRuntimeWorldPropertyType::Object
		|| Texture->ObjectIdentity.IsEmpty() || !FindTexture)
	{
		Error = TEXT("a sprite decoration has no resolvable Texture property");
		return false;
	}
	FString Identity = CanonicalObjectIdentity(Texture->ObjectIdentity);
	TSet<FString> Seen;
	for (int32 FrameIndex = 0; FrameIndex < 1024; ++FrameIndex)
	{
		if (Seen.Contains(Identity)) break;
		Seen.Add(Identity);
		FWoTRetailTextureFacts Facts;
		if (!FindTexture(Identity, Facts) || Facts.USize == 0
			|| Facts.VSize == 0)
		{
			Error = FString::Printf(
				TEXT("sprite decoration texture is unavailable (%s)"), *Identity);
			return false;
		}
		if (OutIdentities.IsEmpty())
		{
			OutWidth = static_cast<float>(Facts.USize);
			OutHeight = static_cast<float>(Facts.VSize);
			OutMinRate = Facts.MinFrameRate;
			OutMaxRate = Facts.MaxFrameRate;
		}
		OutIdentities.Add(Identity);
		if (Facts.AnimNext.IsEmpty()) break;
		Identity = Facts.AnimNext;
	}
	if (OutIdentities.IsEmpty())
	{
		Error = TEXT("a sprite decoration texture animation is empty");
		return false;
	}
	return true;
}

bool ProjectDecoration(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform Transform;
	FWoTRuntimeActorVisualPresentation Visual;
	uint8 DrawType = 2;
	if (!ActorTransform(D, A, Transform)
		|| !WoTRuntimeReadActorVisualPresentation(D, A, Visual, Error)
		|| !ReadByte(D, A, TEXT("DrawType"), DrawType)
		|| (DrawType != 1 && !Visual.HasMesh()))
	{
		if (Error.IsEmpty()) Error = TEXT("a decoration has no effective mesh");
		return false;
	}
	// UE1 collision cylinders are always world-axis-aligned. Keep the actor's
	// collision root unrotated and carry the complete authored pitch/yaw/roll on
	// the display component only; yaw-dependent presentation is preserved.
	const FQuat AuthoredVisualRotation = Transform.GetRotation();
	Transform.SetRotation(FQuat::Identity);
	const bool bBreakable =
		D.ClassIsA(A.ClassIdentity, TEXT("WOT.BreakableDecoration"));
	const bool bBounceable =
		D.ClassIsA(A.ClassIdentity, TEXT("WOT.BounceableDecoration"));
	AWoTDecoration* Decoration = bBreakable
		? static_cast<AWoTDecoration*>(SpawnDeferred<AWoTBreakableDecoration>(
			World, Index, DecorationRule, Transform))
		: bBounceable
			? static_cast<AWoTDecoration*>(SpawnDeferred<AWoTBounceableDecoration>(
				World, Index, DecorationRule, Transform))
			: SpawnDeferred<AWoTDecoration>(World, Index, DecorationRule, Transform);
	if (!Decoration)
	{
		Error = TEXT("a decoration projection could not be allocated"); return false;
	}
	Decoration->Mesh->SetRelativeRotation(AuthoredVisualRotation);
	Decoration->SetReplicates(bBreakable || bBounceable);
	if (bBreakable || bBounceable) Decoration->SetNetAddressable();
	Decoration->ConfigureRuntimeVisualPresentation(Visual);
	if (DrawType == 1)
	{
		TArray<FString> TextureIdentities;
		float TextureWidth = 0.0f;
		float TextureHeight = 0.0f;
		float MinFrameRate = 0.0f;
		float MaxFrameRate = 0.0f;
		if (!WoTProjectDecorationSpriteSeries(Source, D, A, TextureIdentities,
			TextureWidth, TextureHeight, MinFrameRate, MaxFrameRate, Error))
		{
			Decoration->Destroy();
			return false;
		}
		Decoration->ConfigureRuntimeSpritePresentation(MoveTemp(TextureIdentities),
			TextureWidth, TextureHeight, MinFrameRate, MaxFrameRate);
	}
	bool bCollideActors = true;
	bool bBlockActors = true;
	bool bBlockPlayers = true;
	uint8 Physics = 0;
	FRotator RotationRate = FRotator::ZeroRotator;
	bool bFixedRotationDir = false;
	float CollisionRadius = Decoration->Collision->GetUnscaledCapsuleRadius();
	float CollisionHeight = Decoration->Collision->GetUnscaledCapsuleHalfHeight();
	if (!ReadBool(D, A, TEXT("bAutoAnimate"), Decoration->bAutoAnimate)
		|| !ReadBool(D, A, TEXT("bProjTarget"), Decoration->bProjTarget)
		|| !ReadBool(D, A, TEXT("bCollideActors"), bCollideActors)
		|| !ReadBool(D, A, TEXT("bBlockActors"), bBlockActors)
		|| !ReadBool(D, A, TEXT("bBlockPlayers"), bBlockPlayers)
		|| !ReadByte(D, A, TEXT("Physics"), Physics)
		|| !ReadRotator(D, A, TEXT("RotationRate"), RotationRate)
		|| !ReadBool(D, A, TEXT("bFixedRotationDir"), bFixedRotationDir)
		|| !ReadFloat(D, A, TEXT("CollisionRadius"), CollisionRadius)
		|| !ReadFloat(D, A, TEXT("CollisionHeight"), CollisionHeight)
		|| !ReadName(D, A, TEXT("AnimationSequence"), Decoration->AnimationSequence)
		|| !ReadFloat(D, A, TEXT("MinAnimRate"), Decoration->MinAnimRate)
		|| !ReadFloat(D, A, TEXT("MaxAnimRate"), Decoration->MaxAnimRate)
		|| CollisionRadius < 0.0f || CollisionHeight < 0.0f)
	{
		Error = TEXT("a decoration animation property is malformed");
		Decoration->Destroy(); return false;
	}
	Decoration->ConfigureRuntimeCollision(bCollideActors, bBlockActors,
		bBlockPlayers, CollisionRadius, CollisionHeight);
	Decoration->ConfigureRuntimePhysics(
		Physics, RotationRate, bFixedRotationDir);
	FName RenderIteratorClass;
	float VisibilityRadius = 0.0f;
	float VisibilityHeight = 0.0f;
	if (!ReadObjectIdentity(D, A, TEXT("RenderIteratorClass"),
		RenderIteratorClass)
		|| !ReadFloat(D, A, TEXT("VisibilityRadius"), VisibilityRadius)
		|| !ReadFloat(D, A, TEXT("VisibilityHeight"), VisibilityHeight)
		|| VisibilityRadius < 0.0f || VisibilityHeight < 0.0f)
	{
		Error = TEXT("a decoration render iterator property is malformed");
		Decoration->Destroy(); return false;
	}
	if (RenderIteratorClass.ToString().EndsWith(TEXT("DistanceFadeRI"),
		ESearchCase::IgnoreCase))
	{
		Decoration->ConfigureRuntimeDistanceFade(
			VisibilityRadius, VisibilityHeight);
	}
	if (AWoTBounceableDecoration* Bounceable =
		Cast<AWoTBounceableDecoration>(Decoration))
	{
		const FString BounceLeaf = Leaf(A.ClassIdentity);
		if (BounceLeaf.Equals(TEXT("Skull"), ESearchCase::IgnoreCase)
			|| BounceLeaf.Equals(TEXT("SkullSkin"), ESearchCase::IgnoreCase))
			Bounceable->LandSound1 = TEXT("WOT/GibBounceBone1");
		else if (BounceLeaf.Equals(TEXT("OpenBookA"), ESearchCase::IgnoreCase))
		{
			Bounceable->LandSound1 = TEXT("WoTDecorations/Book5"); Bounceable->Mass = 10.0f;
		}
		else if (BounceLeaf.Equals(TEXT("ClosedBookA"), ESearchCase::IgnoreCase))
		{
			Bounceable->LandSound1 = TEXT("WoTDecorations/Book7"); Bounceable->Mass = 10.0f;
		}
		else if (BounceLeaf.StartsWith(TEXT("Chunk"), ESearchCase::IgnoreCase))
			Bounceable->Mass = 800.0f;
		auto OptionalFloat = [&](const TCHAR* Name, float& Value)
		{
			return Prop(D,A,Name) == nullptr || ReadFloat(D,A,Name,Value);
		};
		auto OptionalName = [&](const TCHAR* Name, FName& Value)
		{
			return Prop(D,A,Name) == nullptr || ReadName(D,A,Name,Value);
		};
		auto OptionalSound = [&](const TCHAR* Name, FName& Value)
		{
			return Prop(D,A,Name) == nullptr || ReadObjectSubPath(D,A,Name,Value);
		};
		if (!OptionalFloat(TEXT("Damage"),Bounceable->Damage)
			|| !OptionalFloat(TEXT("MomentumTransfer"),Bounceable->MomentumTransfer)
			|| !OptionalName(TEXT("DamageType"),Bounceable->DamageType)
			|| !OptionalFloat(TEXT("Mass"),Bounceable->Mass)
			|| !OptionalSound(TEXT("LandSound1"),Bounceable->LandSound1)
			|| !OptionalFloat(TEXT("MinBounceSoundSpeed"),Bounceable->MinBounceSoundSpeed)
			|| !OptionalFloat(TEXT("MinFirstHitSpeed"),Bounceable->MinFirstHitSpeed)
			|| !OptionalFloat(TEXT("MinBounceSpeed"),Bounceable->MinBounceSpeed)
			|| !OptionalFloat(TEXT("SpeedDampeningRatio"),Bounceable->SpeedDampeningRatio)
			|| !OptionalFloat(TEXT("RotationDampeningRatio"),Bounceable->RotationDampeningRatio)
			|| !OptionalFloat(TEXT("VolumePerturbPercent"),Bounceable->VolumePerturbPercent)
			|| !OptionalFloat(TEXT("PitchPerturbPercent"),Bounceable->PitchPerturbPercent)
			|| !OptionalFloat(TEXT("BaseBounceVolume"),Bounceable->BaseBounceVolume)
			|| !OptionalFloat(TEXT("BaseBouncePitch"),Bounceable->BaseBouncePitch)
			|| !OptionalFloat(TEXT("LandedYaw"),Bounceable->LandedYaw)
			|| !OptionalFloat(TEXT("LandedRoll"),Bounceable->LandedRoll)
			|| !OptionalFloat(TEXT("LandedPitch"),Bounceable->LandedPitch))
		{
			Error = TEXT("a bounceable decoration property is malformed");
			Decoration->Destroy(); return false;
		}
	}
	const FString ClassLeaf = Leaf(A.ClassIdentity);
	if (ClassLeaf.Equals(TEXT("DragonFly"), ESearchCase::IgnoreCase))
	{
		Decoration->bAutoAnimate = true;
		Decoration->AnimationSequence = TEXT("DRAGONFLY");
	}
	else if (ClassLeaf.Equals(TEXT("Fish"), ESearchCase::IgnoreCase))
	{
		Decoration->PlayAnimCycle = {
			TEXT("Frame01"), TEXT("Frame02"), TEXT("Frame01"), TEXT("Frame03")};
	}
	else if (!Decoration->bAutoAnimate)
	{
		if (!ReadName(D, A, TEXT("AnimSequence"), Decoration->PoseSequence)
			|| !ReadFloat(D, A, TEXT("AnimFrame"), Decoration->PoseFrame))
		{
			Error = TEXT("a decoration pose is malformed");
			Decoration->Destroy(); return false;
		}
	}
	AWoTBreakableDecoration* Breakable =
		Cast<AWoTBreakableDecoration>(Decoration);
	if (Breakable)
	{
		if (!ReadInt(D, A, TEXT("Health"), Breakable->Health)
			|| !ReadFloat(D, A, TEXT("fragmentSize"), Breakable->FragmentSize)
			|| !ReadInt(D, A, TEXT("MinDamageVelocityZ"),
				Breakable->MinDamageVelocityZ)
			|| !ReadBool(D, A, TEXT("bOnlyTriggerable"),
				Breakable->bOnlyTriggerable)
			|| !ReadBool(D, A, TEXT("bPushable"), Breakable->bPushable)
			|| !ReadFloat(D, A, TEXT("Mass"), Breakable->Mass)
			|| !ReadName(D, A, TEXT("Event"), Breakable->EventName))
		{
			Error = TEXT("a breakable decoration property is malformed");
			Decoration->Destroy(); return false;
		}
		// UE1 Actor.EPhysics: PHYS_Falling is 2. Map projection precedes the final
		// client world context/net driver, so defer activation until BeginPlay can
		// make the authoritative choice. Replicated movement then publishes one
		// final rest transform instead of clients racing their own gravity.
		constexpr uint8 PhysicsFalling = 2;
		Breakable->ConfigureInitialFalling(Physics == PhysicsFalling);
		Count(Out, BreakableDecorationRule);
	}
	if (bBounceable) Count(Out, BounceableDecorationRule);
	Out.DeferredActors.Add(Decoration);
	Count(Out, DecorationRule);
	return true;
}

bool ProjectDecalManager(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D, A, Transform))
	{
		Error = TEXT("a decal manager transform is malformed"); return false;
	}
	AWoTDecalManager* Manager = SpawnDeferred<AWoTDecalManager>(
		World, Index, DecalManagerRule, Transform);
	if (!Manager)
	{
		Error = TEXT("a decal manager projection could not be allocated"); return false;
	}
	if (!ReadName(D,A,TEXT("Tag"),Manager->TagName)
		|| !ReadInt(D,A,TEXT("MaxDecals"),Manager->MaxDecals)
		|| !ReadFloat(D,A,TEXT("ShrinkTime"),Manager->ShrinkTime)
		|| !ReadFloat(D,A,TEXT("DecalExpireTime"),Manager->DecalExpireTime)
		|| Manager->MaxDecals < 0 || Manager->ShrinkTime < 0.0f
		|| Manager->DecalExpireTime <= 0.0f)
	{
		Error = TEXT("a decal manager property is malformed");
		Manager->Destroy(); return false;
	}
	Manager->Tags.AddUnique(Manager->TagName);
	Manager->SetNetAddressable();
	Out.DeferredActors.Add(Manager);
	Count(Out, DecalManagerRule);
	return true;
}

bool ProjectExplodingWall(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D, A, Transform))
	{
		Error = TEXT("an exploding wall transform is malformed"); return false;
	}
	AWoTExplodingWall* Wall = SpawnDeferred<AWoTExplodingWall>(
		World, Index, ExplodingWallRule, Transform);
	if (!Wall)
	{
		Error = TEXT("an exploding wall projection could not be allocated"); return false;
	}
	float Radius = Wall->Capsule->GetUnscaledCapsuleRadius();
	float Height = Wall->Capsule->GetUnscaledCapsuleHalfHeight();
	if (!ReadFloat(D,A,TEXT("ExplosionSize"),Wall->ExplosionSize)
		|| !ReadFloat(D,A,TEXT("ExplosionDimensions"),Wall->ExplosionDimensions)
		|| !ReadFloat(D,A,TEXT("WallParticleSize"),Wall->WallParticleSize)
		|| !ReadFloat(D,A,TEXT("WoodParticleSize"),Wall->WoodParticleSize)
		|| !ReadFloat(D,A,TEXT("GlassParticleSize"),Wall->GlassParticleSize)
		|| !ReadInt(D,A,TEXT("NumWallChunks"),Wall->NumWallChunks)
		|| !ReadInt(D,A,TEXT("NumWoodChunks"),Wall->NumWoodChunks)
		|| !ReadInt(D,A,TEXT("NumGlassChunks"),Wall->NumGlassChunks)
		|| !ReadInt(D,A,TEXT("Health"),Wall->Health)
		|| !ReadObjectLeafName(D,A,TEXT("WallTexture"),Wall->WallTextureName)
		|| !ReadObjectLeafName(D,A,TEXT("WoodTexture"),Wall->WoodTextureName)
		|| !ReadObjectLeafName(D,A,TEXT("GlassTexture"),Wall->GlassTextureName)
		|| !ReadObjectSubPath(D,A,TEXT("BreakingSound"),Wall->BreakingSound)
		|| !ReadBool(D,A,TEXT("bTranslucentGlass"),Wall->bTranslucentGlass)
		|| !ReadBool(D,A,TEXT("bUnlitGlass"),Wall->bUnlitGlass)
		|| !ReadBool(D,A,TEXT("bOnlyTriggerable"),Wall->bOnlyTriggerable)
		|| !ReadNameSlots(D,A,TEXT("ActivatedBy"),5,Wall->ActivatedBy,true)
		|| !ReadName(D,A,TEXT("Tag"),Wall->TagName)
		|| !ReadName(D,A,TEXT("Event"),Wall->EventName)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| Radius <= 0.0f || Height <= 0.0f)
	{
		Error = TEXT("an exploding wall property is malformed");
		Wall->Destroy(); return false;
	}
	Wall->Capsule->SetCylinderSize(Radius, Height);
	Wall->SetReplicates(true);
	Wall->SetNetAddressable();
	Out.DeferredActors.Add(Wall);
	Count(Out, ExplodingWallRule);
	return true;
}

bool ProjectDestroyDirective(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D, A, Transform))
	{
		Error = TEXT("a destroy directive transform is malformed"); return false;
	}
	AWoTDestroyDirective* Directive = SpawnDeferred<AWoTDestroyDirective>(
		World, Index, DestroyDirectiveRule, Transform);
	if (!Directive)
	{
		Error = TEXT("a destroy directive projection could not be allocated");
		return false;
	}
	if (!ReadName(D,A,TEXT("Tag"),Directive->TagName)
		|| !ReadObjectLeafSlots(D,A,TEXT("ObserverClasses"),16,
			Directive->ObserverClasses)
		|| !ReadNameSlots(D,A,TEXT("ObserverTags"),16,
			Directive->ObserverTags,true)
		|| !ReadNameSlots(D,A,TEXT("ObserverNames"),16,
			Directive->ObserverNames,true))
	{
		Error = TEXT("a destroy directive property is malformed");
		Directive->Destroy(); return false;
	}
	if (!Directive->ConfigureObserverClasses(D, Error))
	{
		if (Error.IsEmpty()) Error = TEXT("a destroy directive class selector is malformed");
		Directive->Destroy(); return false;
	}
	Out.DeferredActors.Add(Directive);
	Count(Out, DestroyDirectiveRule);
	return true;
}

const FWoTRuntimeMoverModel* FindMoverModel(
	const FWoTWorldProjectionSource& Source, FStringView ActorIdentity)
{
	for (const FWoTRuntimeMoverModel& Model : Source.MoverModels)
		if (FStringView(Model.ActorIdentity).Equals(
			ActorIdentity, ESearchCase::IgnoreCase)) return &Model;
	return nullptr;
}

UClass* NativeMoverClass(FStringView RetailClass)
{
	const FString ClassLeaf = Leaf(RetailClass);
	if (ClassLeaf.Equals(TEXT("DoorMover"), ESearchCase::IgnoreCase)) return AWoTDoorMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("PortcullisMover"), ESearchCase::IgnoreCase)) return AWoTPortcullisMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("ExplodingMover"), ESearchCase::IgnoreCase)) return AWoTExplodingMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("RotatingMover"), ESearchCase::IgnoreCase)) return AWoTRotatingMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("LoopMover"), ESearchCase::IgnoreCase)) return AWoTLoopMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("TimedMover"), ESearchCase::IgnoreCase)) return AWoTTimedMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("AssertMover"), ESearchCase::IgnoreCase)) return AWoTAssertMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("ElevatorMover"), ESearchCase::IgnoreCase)) return AWoTElevatorMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("AttachMover"), ESearchCase::IgnoreCase)) return AWoTAttachMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("GradualMover"), ESearchCase::IgnoreCase)) return AWoTGradualMover::StaticClass();
	if (ClassLeaf.Equals(TEXT("MixMover"), ESearchCase::IgnoreCase)) return AWoTMixMover::StaticClass();
	return AWoTMover::StaticClass();
}

bool BuildMoverMesh(AWoTMover& Mover, const FWoTRuntimeMoverModel& Model,
	bool bDynamicLightMover, bool bHasDedicatedMoverCollision,
	UTexture2D* MoverLightmap,
	TConstArrayView<FWoTRuntimeMoverAnimationGroup> MoverAnimationGroups,
	TConstArrayView<FWoTRuntimeMoverAnimationTerm> MoverAnimationTerms,
	TConstArrayView<UTexture2D*> MoverLightResponses,
	UTexture2D* MoverBlackLightResponse,
	UWoTRuntimeAssets* TextureProviderForTesting, FString& Error)
{
	auto ResolveMoverTexture = [TextureProviderForTesting](
		const FSoftObjectPath& Path,
		const FWoTRuntimeTextureOptions& Options) -> UTexture2D*
	{
#if WITH_AUTOMATION_TESTS
		if (TextureProviderForTesting != nullptr)
			return TextureProviderForTesting->ResolveTexture(Path, Options);
#else
		(void)TextureProviderForTesting;
#endif
		return UWoTRuntimeAssets::ResolveTexturePath(Path, Options);
	};
	const bool bBakedLightmap = Model.bLightmapped && !bDynamicLightMover;
	if (bBakedLightmap
		&& (MoverLightmap == nullptr || Model.UV1.Num() != Model.UV0.Num()))
	{
		Error = TEXT("a baked mover has no complete authenticated light-map");
		return false;
	}
	if (MoverAnimationTerms.Num() != MoverLightResponses.Num())
	{
		Error = TEXT("the mover animation terms have no parallel response textures");
		return false;
	}
	FWoTBSPPayloadData Payload;
	Payload.FormatVersion = 4;
	Payload.Positions = Model.Positions;
	Payload.Normals = Model.Normals;
	Payload.UV0 = Model.UV0;
	if (bBakedLightmap) Payload.UV1 = Model.UV1;
	else Payload.UV1.SetNumZeroed(Model.UV0.Num());
	Payload.Indices = Model.Indices;
	if (!Payload.Positions.IsEmpty())
	{
		Payload.BoundsMin = Payload.BoundsMax = Payload.Positions[0];
		for (const FVector3f Position : Payload.Positions)
		{
			Payload.BoundsMin = Payload.BoundsMin.ComponentMin(Position);
			Payload.BoundsMax = Payload.BoundsMax.ComponentMax(Position);
		}
	}
	for (const FWoTRuntimeMoverSection& Section : Model.Sections)
	{
		FWoTBSPPayloadSection& Added = Payload.Sections.AddDefaulted_GetRef();
		Added.MaterialSlot = Section.MaterialSlot;
		Added.FirstIndex = Section.FirstIndex;
		Added.IndexCount = Section.IndexCount;
		Added.MinVertex = Section.MinVertex;
		Added.MaxVertex = Section.MaxVertex;
		Added.bCollisionEnabled = Section.bCollisionEnabled;
		Added.CanonicalPackage = Section.CanonicalPackage;
		Added.CanonicalName = Section.CanonicalName;
		Added.SourceTextureIdentity = Section.SourceTextureIdentity;
		Added.SurfaceFlags = Section.SurfaceFlags;
		Added.MacroCanonicalPackage = Section.MacroCanonicalPackage;
		Added.MacroCanonicalName = Section.MacroCanonicalName;
		Added.MacroSourceTextureIdentity = Section.MacroSourceTextureIdentity;
		Added.MacroRatio = Section.MacroRatio;
	}
	FWoTBSPPayloadData CollisionPayload;
	CollisionPayload.FormatVersion = 4;
	// World-v2/v3 have no dedicated collision stream and retain their original
	// render-triangle behavior. In v4+, an empty stream is authoritative: a
	// wholly PF_NotSolid mover renders but owns no collision body.
	CollisionPayload.Positions = !bHasDedicatedMoverCollision
		&& Model.CollisionPositions.IsEmpty()
		? Model.Positions : Model.CollisionPositions;
	CollisionPayload.Indices = !bHasDedicatedMoverCollision
		&& Model.CollisionIndices.IsEmpty()
		? Model.Indices : Model.CollisionIndices;
	CollisionPayload.Normals.Init(
		FVector3f::ZAxisVector, CollisionPayload.Positions.Num());
	CollisionPayload.UV0.SetNumZeroed(CollisionPayload.Positions.Num());
	CollisionPayload.UV1.SetNumZeroed(CollisionPayload.Positions.Num());
	if (!CollisionPayload.Positions.IsEmpty())
	{
		CollisionPayload.BoundsMin = CollisionPayload.BoundsMax =
			CollisionPayload.Positions[0];
		for (const FVector3f Position : CollisionPayload.Positions)
		{
			CollisionPayload.BoundsMin =
				CollisionPayload.BoundsMin.ComponentMin(Position);
			CollisionPayload.BoundsMax =
				CollisionPayload.BoundsMax.ComponentMax(Position);
		}
	}
	for (int32 At = 0; At + 2 < CollisionPayload.Indices.Num(); At += 3)
	{
		const uint32 A = CollisionPayload.Indices[At];
		const uint32 B = CollisionPayload.Indices[At + 1];
		const uint32 C = CollisionPayload.Indices[At + 2];
		if (!CollisionPayload.Positions.IsValidIndex(A)
			|| !CollisionPayload.Positions.IsValidIndex(B)
			|| !CollisionPayload.Positions.IsValidIndex(C))
		{
			Error = TEXT("a mover collision triangle references a missing vertex");
			return false;
		}
		const FVector3f Normal = FVector3f::CrossProduct(
			CollisionPayload.Positions[B] - CollisionPayload.Positions[A],
			CollisionPayload.Positions[C] - CollisionPayload.Positions[A])
			.GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Error = TEXT("a mover collision triangle is degenerate");
			return false;
		}
		CollisionPayload.Normals[A] = Normal;
		CollisionPayload.Normals[B] = Normal;
		CollisionPayload.Normals[C] = Normal;
	}
	const bool bHasCollision = !CollisionPayload.Indices.IsEmpty();
	if (bHasCollision)
	{
		FWoTBSPPayloadSection& CollisionSection =
			CollisionPayload.Sections.AddDefaulted_GetRef();
		CollisionSection.MaterialSlot = TEXT("__MoverCollision");
		CollisionSection.IndexCount = CollisionPayload.Indices.Num();
		CollisionSection.MaxVertex = CollisionPayload.Positions.Num() - 1;
		CollisionSection.bCollisionEnabled = true;
	}

	// A mover is an independent coordinate/model domain. Repair its render
	// copy only: both dedicated and legacy fallback collision above still
	// originate from the untouched Model streams.
	TArray<FWoTBSPPayloadData> RenderParts;
	RenderParts.Add(MoveTemp(Payload));
	FWoTBSPTJunctionStats RepairStats;
	FString RepairError;
	if (!WoTRuntimeBSPRepairRenderTJunctions(
		RenderParts, RepairStats, RepairError))
	{
		// A mover's render repair is likewise optional presentation work. The
		// transaction retained its original render payload and collision streams.
		UE_LOG(LogTemp, Warning, TEXT("%s: %s; using unrepaired mover render geometry"),
			*GetNameSafe(&Mover), *RepairError);
	}
	else if (RepairStats.bFellBackToUnrepaired)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("%s: mover render repair retained %llu source triangles; first=%s"),
			*GetNameSafe(&Mover), RepairStats.FallbackTriangles,
			*RepairStats.FallbackReason);
	}
	Payload = MoveTemp(RenderParts[0]);

	Mover.RuntimeMesh = NewObject<UStaticMesh>(
		&Mover, TEXT("RuntimeMoverMesh"), RF_Transient);
	if (!Mover.RuntimeMesh
		|| !WoTRuntimeBSPBuildRenderMesh(*Mover.RuntimeMesh, Payload, Error)
		|| (bHasCollision && !WoTRuntimeBSPBuildCollision(
			*Mover.RuntimeMesh, CollisionPayload, Error)))
	{
		if (Error.IsEmpty()) Error = TEXT("runtime mover mesh construction failed");
		return false;
	}
	Mover.Mesh->SetStaticMesh(Mover.RuntimeMesh);
	Mover.Mesh->SetCollisionProfileName(
		bHasCollision ? TEXT("BlockAllDynamic") : TEXT("NoCollision"));
	Mover.Mesh->SetCollisionObjectType(ECC_WorldDynamic);

	for (int32 SectionIndex = 0; SectionIndex < Model.Sections.Num(); ++SectionIndex)
	{
		const FWoTRuntimeMoverSection& Section = Model.Sections[SectionIndex];
		FWoTRuntimeBSPSectionStyle Style;
		const bool bNullTexture = Section.CanonicalPackage.IsEmpty()
			&& (Section.CanonicalName.IsEmpty()
				|| Section.CanonicalName.Equals(TEXT("None"),ESearchCase::IgnoreCase))
			&& (Section.SourceTextureIdentity.IsEmpty()
				|| Section.SourceTextureIdentity.Equals(TEXT("None"),ESearchCase::IgnoreCase));
		if (!bNullTexture && !WoTRuntimeBSPSectionStyleForIdentity(
			Section.CanonicalPackage, Section.CanonicalName,
			Section.SurfaceFlags, Style))
		{
			Error = FString::Printf(
				TEXT("runtime mover material identity is malformed: package=%s name=%s source=%s"),
				*Section.CanonicalPackage,*Section.CanonicalName,
				*Section.SourceTextureIdentity);
			return false;
		}
		UTexture2D* Texture = nullptr;
		FWoTRuntimeTextureOptions TextureOptions;
		TextureOptions.bLoadMips = true;
		TextureOptions.bSRGB = false;
		// Movers filter like world BSP (WoTRuntimeBSPMaterializeTexture).
		TextureOptions.Filter = Style.bNoSmooth ? TF_Nearest : TF_Default;
		TextureOptions.LODBias = -1;
		TextureOptions.AddressX = TA_Wrap;
		TextureOptions.AddressY = TA_Wrap;
		TextureOptions.bForcePaletteIndexZeroTransparent =
			Style.Blend == EWoTRuntimeBSPBlend::Masked;
		if (!Section.CanonicalPackage.IsEmpty() && !Section.CanonicalName.IsEmpty())
		{
			// Exact identity of a name that collides inside its package: the
			// canonical package/name plus the groups of the section's exact
			// source identity (WoTRetailIdentity.h).
			FWoTRetailObjectPath MoverTexture;
			FWoTRetailObjectPath MoverSource;
			if (WoTParseRetailObjectPath(Section.SourceTextureIdentity, MoverSource))
				MoverTexture.Groups = MoveTemp(MoverSource.Groups);
			MoverTexture.Package = Section.CanonicalPackage;
			MoverTexture.Name = Section.CanonicalName;
			const FString TexturePath = FString::Printf(
				TEXT("/Game/WoT/Textures/%s.%s"),
				*WoTRetailTextureSubPath(MoverTexture), *Section.CanonicalName);
			Texture = ResolveMoverTexture(
				FSoftObjectPath(TexturePath), TextureOptions);
			if (!Texture)
			{
				UE_LOG(LogTemp, Display,
					TEXT("Mover texture '%s' is unavailable; substituting retail Engine.DefaultTexture"),
					*TexturePath);
			}
		}
		if (!Texture)
		{
			Texture = ResolveMoverTexture(FSoftObjectPath(
				TEXT("/Game/WoT/Textures/Engine/DefaultTexture.DefaultTexture")),
				TextureOptions);
		}
		if (!Texture)
		{
			Error = TEXT("retail Engine.DefaultTexture is unavailable for a mover surface");
			return false;
		}
		WoTRuntimeBSPApplyTextureFlags(
			Style, UWoTRuntimeAssets::ResolveTextureMasked(Texture));
		UMaterialInterface* Master = nullptr;
		if (bBakedLightmap)
		{
			if (Section.bAnimatedLightmap)
			{
				const TCHAR* MasterName =
					Style.Blend == EWoTRuntimeBSPBlend::Translucent
						? TEXT("MI_WoT_MoverWaveRuntimeTranslucent")
						: Style.Blend == EWoTRuntimeBSPBlend::Modulated
							? TEXT("MI_WoT_MoverWaveRuntimeModulated")
							: Style.Blend == EWoTRuntimeBSPBlend::Masked
								? TEXT("MI_WoT_MoverWaveRuntimeMasked")
								: TEXT("MI_WoT_MoverWaveRuntime");
				Master = LoadObject<UMaterialInterface>(nullptr,
					*FString::Printf(TEXT("/Game/WoT/Materials/%s.%s"),
						MasterName, MasterName));
			}
			else
			{
				Master = WoTWorldRuntimeBSPMaster(Style, false);
			}
		}
		else
		{
			const TCHAR* MasterName = Style.Blend == EWoTRuntimeBSPBlend::Translucent
				? TEXT("M_WoT_MeshLitT")
				: Style.Blend == EWoTRuntimeBSPBlend::Masked
					? TEXT("M_WoT_MoverLitMasked") : TEXT("M_WoT_MoverLit");
			Master = LoadObject<UMaterialInterface>(nullptr,
				*FString::Printf(TEXT("/Game/WoT/Materials/%s.%s"),
					MasterName, MasterName));
		}
		UMaterialInstanceDynamic* Material = Master
			? UMaterialInstanceDynamic::Create(Master, &Mover) : nullptr;
		if (!Material)
		{
			Error = TEXT("runtime mover material master is absent");
			return false;
		}
		Mover.RuntimeTextures.AddUnique(Texture);
		Material->SetTextureParameterValue(TEXT("Tex"), Texture);
		if (bBakedLightmap)
		{
			Mover.RuntimeTextures.AddUnique(MoverLightmap);
			Material->SetTextureParameterValue(TEXT("Lightmap"), MoverLightmap);
			Material->SetScalarParameterValue(TEXT("PanU"), 0.0f);
			Material->SetScalarParameterValue(TEXT("PanV"), 0.0f);
			Material->SetScalarParameterValue(TEXT("WoTSceneTime"), 0.0f);
			Material->SetScalarParameterValue(
				TEXT("DynLightEnable"), Style.bSpecialLit ? 0.0f : 1.0f);
			if (Section.bAnimatedLightmap)
			{
				if (MoverBlackLightResponse == nullptr
					|| MoverAnimationTerms.Num() > 6
					|| MoverAnimationGroups.Num() > 4)
				{
					Error = TEXT("an animated mover has incomplete response material data");
					return false;
				}
				Material->SetScalarParameterValue(
					TEXT("LightmapWidth"), MoverLightmap->GetSizeX());
				Material->SetScalarParameterValue(
					TEXT("LightmapHeight"), MoverLightmap->GetSizeY());
				for (int32 ResponseIndex = 0; ResponseIndex < 6; ++ResponseIndex)
				{
					const bool bUsed = MoverAnimationTerms.IsValidIndex(ResponseIndex);
					const FWoTRuntimeMoverAnimationTerm* Term = bUsed
						? &MoverAnimationTerms[ResponseIndex] : nullptr;
					UTexture2D* Response = bUsed
						? MoverLightResponses[ResponseIndex]
						: MoverBlackLightResponse;
					if (Response == nullptr)
					{
						Error = TEXT("an animated mover response texture is absent");
						return false;
					}
					Mover.RuntimeTextures.AddUnique(Response);
					Material->SetTextureParameterValue(
						*FString::Printf(TEXT("Delta%d"), ResponseIndex), Response);
					Material->SetScalarParameterValue(
						*FString::Printf(TEXT("DeltaWaveIndex%d"), ResponseIndex),
						Term != nullptr ? static_cast<float>(Term->Group) : -1.0f);
					Material->SetScalarParameterValue(
						*FString::Printf(TEXT("DeltaEffectSalt%d"), ResponseIndex),
						Term != nullptr ? static_cast<float>(Term->Effect) : 0.0f);
				}
			}
			const bool bMacroMaster = Style.Blend == EWoTRuntimeBSPBlend::Opaque
				|| Style.Blend == EWoTRuntimeBSPBlend::Masked;
			if (bMacroMaster && !Section.MacroCanonicalPackage.IsEmpty()
				&& !Section.MacroCanonicalName.IsEmpty()
				&& !Section.MacroSourceTextureIdentity.IsEmpty()
				&& Section.MacroRatio > 0.0f)
			{
				FWoTRetailObjectPath MacroTextureIdentity;
				FWoTRetailObjectPath MacroSourceIdentity;
				if (WoTParseRetailObjectPath(
					Section.MacroSourceTextureIdentity, MacroSourceIdentity))
				{
					MacroTextureIdentity.Groups = MoveTemp(MacroSourceIdentity.Groups);
				}
				MacroTextureIdentity.Package = Section.MacroCanonicalPackage;
				MacroTextureIdentity.Name = Section.MacroCanonicalName;
				const FString MacroTexturePath = FString::Printf(
					TEXT("/Game/WoT/Textures/%s.%s"),
					*WoTRetailTextureSubPath(MacroTextureIdentity),
					*Section.MacroCanonicalName);
				UTexture2D* MacroTexture = ResolveMoverTexture(
					FSoftObjectPath(MacroTexturePath), TextureOptions);
				if (MacroTexture != nullptr)
				{
					Mover.RuntimeTextures.AddUnique(MacroTexture);
					Material->SetTextureParameterValue(TEXT("MacroTex"), MacroTexture);
					Material->SetScalarParameterValue(
						TEXT("MacroRatio"), Section.MacroRatio);
				}
			}
			// UE1 DetailTexture, as on world sections (WoTWorldMaterializer):
			// mover polys are never portals (the reader skips PF_Portal).
			FWoTRuntimeBSPDetailTexture Detail;
			if (bMacroMaster && WoTRuntimeBSPFindDetailTexture(
				Section.CanonicalPackage, Section.CanonicalName, Detail))
			{
				FWoTRetailObjectPath DetailTextureIdentity;
				FWoTRetailObjectPath DetailSourceIdentity;
				if (WoTParseRetailObjectPath(
					Detail.SourceTextureIdentity, DetailSourceIdentity))
				{
					DetailTextureIdentity.Groups =
						MoveTemp(DetailSourceIdentity.Groups);
				}
				DetailTextureIdentity.Package = Detail.CanonicalPackage;
				DetailTextureIdentity.Name = Detail.CanonicalName;
				const FString DetailTexturePath = FString::Printf(
					TEXT("/Game/WoT/Textures/%s.%s"),
					*WoTRetailTextureSubPath(DetailTextureIdentity),
					*Detail.CanonicalName);
				UTexture2D* DetailTexture = ResolveMoverTexture(
					FSoftObjectPath(DetailTexturePath), TextureOptions);
				if (DetailTexture != nullptr)
				{
					Mover.RuntimeTextures.AddUnique(DetailTexture);
					Material->SetTextureParameterValue(TEXT("DetailTex"), DetailTexture);
					Material->SetScalarParameterValue(
						TEXT("DetailRatio"), Detail.RatioU);
					Material->SetScalarParameterValue(
						TEXT("DetailRatioV"), Detail.RatioV);
				}
			}
		}
		else if (Style.Blend != EWoTRuntimeBSPBlend::Translucent)
			Material->SetScalarParameterValue(TEXT("MLSurfScale"), 2.0f);
		Mover.RuntimeMaterials.Add(Material);
		Mover.Mesh->SetMaterial(SectionIndex, Material);

		if (bDynamicLightMover)
		{
			FWoTMoverSurfaceLight& Surface =
				Mover.MeshLight->MoverSurfaces.AddDefaulted_GetRef();
			Surface.MaterialSlot = FName(*Section.MaterialSlot);
			Surface.LocalOrigin = FVector(Section.PlaneOrigin);
			Surface.LocalNormal = FVector(Section.PlaneNormal).GetSafeNormal();
		}
	}
	Mover.MeshLight->bStaticMoverSurfaceLighting =
		!bDynamicLightMover && !bBakedLightmap;
	return true;
}

bool ConfigureMoverProperties(AWoTMover& Mover,
	const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	FString& Error)
{
	int32 HighestKey = 0;
	for (const FWoTRuntimeWorldProperty& Property : A.Properties)
		if (Property.Name.Equals(TEXT("KeyPos"), ESearchCase::IgnoreCase)
			|| Property.Name.Equals(TEXT("KeyRot"), ESearchCase::IgnoreCase))
			HighestKey = FMath::Max(HighestKey, static_cast<int32>(Property.ArrayIndex));
	Mover.KeyPos.SetNumZeroed(HighestKey + 1);
	Mover.KeyRot.SetNumZeroed(HighestKey + 1);
	for (int32 Slot = 0; Slot <= HighestKey; ++Slot)
	{
		const FWoTRuntimeWorldProperty* Position = Prop(D, A, TEXT("KeyPos"), Slot);
		const FWoTRuntimeWorldProperty* Rotation = Prop(D, A, TEXT("KeyRot"), Slot);
		if ((Position && !WoTRuntimeWorldPropertyVector(*Position, Mover.KeyPos[Slot]))
			|| (Rotation && !WoTRuntimeWorldPropertyRotator(*Rotation, Mover.KeyRot[Slot])))
		{
			Error = TEXT("a mover keyframe property is malformed");
			return false;
		}
	}
	FName State;
	uint8 SerializedKeyNum = 0;
	uint8 SerializedBumpType = static_cast<uint8>(EWoTMoverBumpType::PlayerBump);
	uint8 SerializedGlideType = static_cast<uint8>(EWoTMoverGlideType::GlideByTime);
	uint8 SerializedEncroach = 1;
	bool bHidden = false;
	auto Invalid=[&Error](const TCHAR* Property)
	{
		Error=FString::Printf(TEXT("a mover property is malformed: %s"),Property);
		return false;
	};
	if (!ReadFloat(D,A,TEXT("MoveTime"),Mover.MoveTime)) return Invalid(TEXT("MoveTime"));
	if (!ReadFloat(D,A,TEXT("StayOpenTime"),Mover.StayOpenTime)) return Invalid(TEXT("StayOpenTime"));
	if (!ReadFloat(D,A,TEXT("DelayTime"),Mover.DelayTime)) return Invalid(TEXT("DelayTime"));
	if (!ReadByte(D,A,TEXT("KeyNum"),SerializedKeyNum)) return Invalid(TEXT("KeyNum"));
	if (!ReadByte(D,A,TEXT("BumpType"),SerializedBumpType)
		|| SerializedBumpType > static_cast<uint8>(EWoTMoverBumpType::AnyBump))
		return Invalid(TEXT("BumpType"));
	if (!ReadByte(D,A,TEXT("MoverGlideType"),SerializedGlideType)
		|| SerializedGlideType > static_cast<uint8>(EWoTMoverGlideType::GlideByTime))
		return Invalid(TEXT("MoverGlideType"));
	if (!ReadBool(D,A,TEXT("bHidden"),bHidden)) return Invalid(TEXT("bHidden"));
	if (!ReadName(D,A,TEXT("InitialState"),State)) return Invalid(TEXT("InitialState"));
	if (!ReadBool(D,A,TEXT("bTriggerOnceOnly"),Mover.bTriggerOnceOnly)) return Invalid(TEXT("bTriggerOnceOnly"));
	if (!ReadBool(D,A,TEXT("bDamageTriggered"),Mover.bDamageTriggered)) return Invalid(TEXT("bDamageTriggered"));
	if (!ReadByte(D,A,TEXT("MoverEncroachType"),SerializedEncroach)) return Invalid(TEXT("MoverEncroachType"));
	if (!ReadInt(D,A,TEXT("EncroachDamage"),Mover.EncroachDamage)) return Invalid(TEXT("EncroachDamage"));
	if (!ReadBool(D,A,TEXT("bSlave"),Mover.bSlave)) return Invalid(TEXT("bSlave"));
	if (!ReadBool(D,A,TEXT("bSpecialLit"),Mover.bSpecialLit)) return Invalid(TEXT("bSpecialLit"));
	if (!ReadName(D,A,TEXT("ReturnGroup"),Mover.ReturnGroup)) return Invalid(TEXT("ReturnGroup"));
	if (!ReadName(D,A,TEXT("Tag"),Mover.TagName)) return Invalid(TEXT("Tag"));
	if (!ReadName(D,A,TEXT("Event"),Mover.EventName)) return Invalid(TEXT("Event"));
	if (!ReadBool(D,A,TEXT("bLockable"),Mover.bLockable)) return Invalid(TEXT("bLockable"));
	if (!ReadBool(D,A,TEXT("bUnlocked"),Mover.bUnlocked)) return Invalid(TEXT("bUnlocked"));
	if (!ReadBool(D,A,TEXT("bStayUnlocked"),Mover.bStayUnlocked)) return Invalid(TEXT("bStayUnlocked"));
	if (!ReadObjectSubPath(D,A,TEXT("LockedSound"),Mover.LockedSoundPath)) return Invalid(TEXT("LockedSound"));
	if (!ReadObjectSubPath(D,A,TEXT("UnLockSound"),Mover.UnLockSoundPath)) return Invalid(TEXT("UnLockSound"));
	if (!ReadString(D,A,TEXT("LockedMessage"),Mover.LockedMessage))
	{
		const FWoTRuntimeWorldProperty* P=Prop(D,A,TEXT("LockedMessage"));
		Error=FString::Printf(TEXT("a mover property is malformed: LockedMessage type=%d bytes=%d"),
			P?static_cast<int32>(P->Type):-1,P?P->Value.Num():-1);
		return false;
	}
	if (!ReadString(D,A,TEXT("UnLockMessage"),Mover.UnLockMessage)) return Invalid(TEXT("UnLockMessage"));
	if (!ReadFloat(D,A,TEXT("LockedMessageDuration"),Mover.LockedMessageDuration)) return Invalid(TEXT("LockedMessageDuration"));
	if (!ReadFloat(D,A,TEXT("UnLockMessageDuration"),Mover.UnLockMessageDuration)) return Invalid(TEXT("UnLockMessageDuration"));
	if (!ReadFloat(D,A,TEXT("MinLockSoundTime"),Mover.MinLockSoundTime)) return Invalid(TEXT("MinLockSoundTime"));
	if (!ReadFloat(D,A,TEXT("UnLockDuration"),Mover.UnLockDuration)) return Invalid(TEXT("UnLockDuration"));
	if (!ReadObjectSubPath(D,A,TEXT("OpeningSound"),Mover.OpeningSoundPath)) return Invalid(TEXT("OpeningSound"));
	if (!ReadObjectSubPath(D,A,TEXT("OpenedSound"),Mover.OpenedSoundPath)) return Invalid(TEXT("OpenedSound"));
	if (!ReadObjectSubPath(D,A,TEXT("ClosingSound"),Mover.ClosingSoundPath)) return Invalid(TEXT("ClosingSound"));
	if (!ReadObjectSubPath(D,A,TEXT("ClosedSound"),Mover.ClosedSoundPath)) return Invalid(TEXT("ClosedSound"));
	if (!ReadObjectSubPath(D,A,TEXT("MoveAmbientSound"),Mover.MoveAmbientSoundPath)) return Invalid(TEXT("MoveAmbientSound"));
	if (!ReadByte(D,A,TEXT("SoundRadius"),Mover.SoundRadius)) return Invalid(TEXT("SoundRadius"));
	if (!ReadByte(D,A,TEXT("SoundVolume"),Mover.SoundVolume)) return Invalid(TEXT("SoundVolume"));
	if (!ReadByte(D,A,TEXT("SoundPitch"),Mover.SoundPitch)) return Invalid(TEXT("SoundPitch"));
	Mover.EncroachType = SerializedEncroach == 0
		? EWoTMoverEncroachType::StopWhenEncroach
		: SerializedEncroach == 1 ? EWoTMoverEncroachType::ReturnWhenEncroach
		: SerializedEncroach == 2 ? EWoTMoverEncroachType::CrushWhenEncroach
		: EWoTMoverEncroachType::IgnoreWhenEncroach;
	Mover.InitialKeyNum = FMath::Clamp(static_cast<int32>(SerializedKeyNum), 0, 7);
	Mover.BumpType = static_cast<EWoTMoverBumpType>(SerializedBumpType);
	Mover.MoverGlideType = static_cast<EWoTMoverGlideType>(SerializedGlideType);
	Mover.SetActorHiddenInGame(bHidden);
	const FString ClassLeaf = Leaf(A.ClassIdentity);
	if (ClassLeaf.Equals(TEXT("LockableMover"), ESearchCase::IgnoreCase))
		Mover.bLockable = true;
	if (!ClassLeaf.Equals(TEXT("PortcullisMover"), ESearchCase::IgnoreCase))
		Mover.InitialState = MoverStateFromName(State);
	// These classes override Engine.Mover.BeginPlay's authored KeyNum seat.
	if (Cast<AWoTLoopMover>(&Mover) || Cast<AWoTAssertMover>(&Mover)
		|| Cast<AWoTGradualMover>(&Mover) || Cast<AWoTMixMover>(&Mover))
		Mover.InitialKeyNum = 0;
	else if (Cast<AWoTPortcullisMover>(&Mover))
		Mover.InitialKeyNum = 1;
	if (AWoTGradualMover* Gradual = Cast<AWoTGradualMover>(&Mover))
	{
		if (Mover.InitialState == EWoTMoverState::None)
			Gradual->NativeInitialState = State;
		if (!ReadFloatSlots(D,A,TEXT("OpenTimes"),6,Gradual->OpenTimes,false)
			|| !ReadFloatSlots(D,A,TEXT("CloseTimes"),6,Gradual->CloseTimes,false)
			|| !ReadNameSlots(D,A,TEXT("Tags"),6,Gradual->Tags,false)
			|| !ReadNameSlots(D,A,TEXT("Events"),6,Gradual->Events,false))
		{
			Error = TEXT("a gradual mover property is malformed"); return false;
		}
	}
	if (AWoTMixMover* Mix = Cast<AWoTMixMover>(&Mover))
		if (!ReadName(D,A,TEXT("AttachTag"),Mix->AttachTag))
		{ Error=TEXT("a mix mover property is malformed"); return false; }
	if (AWoTAttachMover* Attach = Cast<AWoTAttachMover>(&Mover))
		if (!ReadName(D,A,TEXT("AttachTag"),Attach->AttachTag))
		{ Error=TEXT("an attach mover property is malformed"); return false; }
	if (AWoTRotatingMover* Rotating = Cast<AWoTRotatingMover>(&Mover))
		if (!ReadRotator(D,A,TEXT("RotateRate"),Rotating->RotateRate))
		{ Error=TEXT("a rotating mover property is malformed"); return false; }
	if (AWoTLoopMover* Loop = Cast<AWoTLoopMover>(&Mover))
		if (!ReadInt(D,A,TEXT("NextKeyNum"),Loop->NextKeyNum))
		{ Error=TEXT("a loop mover property is malformed"); return false; }
	if (AWoTTimedMover* Timed = Cast<AWoTTimedMover>(&Mover))
		if (!ReadFloatSlots(D,A,TEXT("Delays"),8,Timed->Delays,true)
			|| !ReadFloatSlots(D,A,TEXT("MoveTimes"),8,Timed->MoveTimes,true)
			|| !ReadNameSlots(D,A,TEXT("Sounds"),8,Timed->Sounds,true,true))
		{ Error=TEXT("a timed mover property is malformed"); return false; }
	if (AWoTAssertMover* Assert = Cast<AWoTAssertMover>(&Mover))
		if (!ReadFloatSlots(D,A,TEXT("OpenTimes"),6,Assert->OpenTimes,true)
			|| !ReadFloatSlots(D,A,TEXT("CloseTimes"),6,Assert->CloseTimes,true)
			|| !ReadBool(D,A,TEXT("bOnceOnlyStopOpen"),Assert->bOnceOnlyStopOpen)
			|| !ReadFloat(D,A,TEXT("WaitUnAssertTime"),Assert->WaitUnAssertTime)
			|| !ReadBool(D,A,TEXT("bCloseImmediately"),Assert->bCloseImmediately)
			|| !ReadInt(D,A,TEXT("LastKeyNum"),Assert->LastKeyNum))
		{ Error=TEXT("an assert mover property is malformed"); return false; }
	if (AWoTExplodingMover* Exploding = Cast<AWoTExplodingMover>(&Mover))
	{
		if (!ReadFloat(D,A,TEXT("ExplosionSize"),Exploding->ExplosionSize)
			|| !ReadFloat(D,A,TEXT("ExplosionDimensions"),Exploding->ExplosionDimensions)
			|| !ReadFloat(D,A,TEXT("WallParticleSize"),Exploding->WallParticleSize)
			|| !ReadFloat(D,A,TEXT("WoodParticleSize"),Exploding->WoodParticleSize)
			|| !ReadFloat(D,A,TEXT("GlassParticleSize"),Exploding->GlassParticleSize)
			|| !ReadInt(D,A,TEXT("NumWallChunks"),Exploding->NumWallChunks)
			|| !ReadInt(D,A,TEXT("NumWoodChunks"),Exploding->NumWoodChunks)
			|| !ReadInt(D,A,TEXT("NumGlassChunks"),Exploding->NumGlassChunks)
			|| !ReadInt(D,A,TEXT("Health"),Exploding->Health)
			|| !ReadObjectLeafName(D,A,TEXT("WallTexture"),Exploding->WallTextureName)
			|| !ReadObjectLeafName(D,A,TEXT("WoodTexture"),Exploding->WoodTextureName)
			|| !ReadObjectLeafName(D,A,TEXT("GlassTexture"),Exploding->GlassTextureName)
			|| !ReadObjectSubPath(D,A,TEXT("BreakingSound"),Exploding->BreakingSound)
			|| !ReadBool(D,A,TEXT("bTranslucentGlass"),Exploding->bTranslucentGlass)
			|| !ReadBool(D,A,TEXT("bUnlitGlass"),Exploding->bUnlitGlass)
			|| !ReadBool(D,A,TEXT("bOnlyTriggerable"),Exploding->bOnlyTriggerable)
			|| !ReadNameSlots(D,A,TEXT("ActivatedBy"),5,Exploding->ActivatedBy,true))
		{ Error=TEXT("an exploding mover property is malformed"); return false; }
	}
	return true;
}

bool ProjectMover(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	int32 Index, FWoTWorldProjectionResult& Out, FString& Error)
{
	const FWoTRuntimeMoverModel* SourceModel = FindMoverModel(Source, A.Identity);
	FTransform Transform;
	FWoTMoverBrushFrame Frame;
	if (!SourceModel || !ReadMoverTransform(D, A, Transform)
		|| !ReadMoverBrushFrame(D, A, Transform, Frame))
	{
		Error = SourceModel ? TEXT("a mover transform is malformed")
			: TEXT("a mover has no authenticated world-v4 model");
		return false;
	}
	// The cached model is brush-local; the actor carries only Location and
	// Rotation, so PrePivot and the scales are applied to the mesh here.
	FWoTRuntimeMoverModel FramedModel = *SourceModel;
	if (!WoTApplyMoverBrushFrame(Frame, FramedModel, Error)) return false;
	const FWoTRuntimeMoverModel* Model = &FramedModel;
	AWoTMover* Mover = Cast<AWoTMover>(SpawnDeferredClass(
		World, *NativeMoverClass(A.ClassIdentity), Index, MoverRule, Transform));
	if (!Mover)
	{ Error=TEXT("a mover projection could not be allocated"); return false; }
	Mover->SetReplicates(true);
	Mover->SetNetAddressable();
	bool bDynamicLightMover = false;
	if (!ReadBool(D,A,TEXT("bDynamicLightMover"),bDynamicLightMover)
		|| !ConfigureMoverProperties(*Mover,D,A,Error)
		|| !BuildMoverMesh(*Mover,*Model,bDynamicLightMover,
			Source.bHasDedicatedMoverCollision, Source.MoverLightmap,
			Source.MoverAnimationGroups, Source.MoverAnimationTerms,
			Source.MoverLightResponses, Source.MoverBlackLightResponse,
			/*TextureProviderForTesting*/ nullptr, Error))
	{
		if (Error.IsEmpty()) Error=TEXT("a mover projection is malformed");
		Mover->Destroy(); return false;
	}
	Mover->ConfigureInitialRestPose(Transform.GetLocation(), Transform.Rotator());
	Out.DeferredActors.Add(Mover);
	Count(Out,MoverRule);
	return true;
}

bool ConfigureTrigger(AWoTTrigger& Trigger,
	const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	FString& Error)
{
	float Radius = Trigger.Capsule->GetUnscaledCapsuleRadius();
	float Height = Trigger.Capsule->GetUnscaledCapsuleHalfHeight();
	bool bCollideActors = true;
	uint8 Type = static_cast<uint8>(Trigger.TriggerType);
	FName ClassIdentity;
	if (!ReadName(D,A,TEXT("Tag"),Trigger.TagName)
		|| !ReadName(D,A,TEXT("Event"),Trigger.EventName)
		|| !ReadBool(D,A,TEXT("bTriggerOnceOnly"),Trigger.bTriggerOnceOnly)
		|| !ReadFloat(D,A,TEXT("ReTriggerDelay"),Trigger.ReTriggerDelay)
		|| !ReadFloat(D,A,TEXT("RepeatTriggerTime"),Trigger.RepeatTriggerTime)
		|| !ReadBool(D,A,TEXT("bCollideActors"),bCollideActors)
		|| !ReadByte(D,A,TEXT("TriggerType"),Type)
		|| !ReadObjectIdentity(D,A,TEXT("ClassProximityType"),ClassIdentity)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| Radius <= 0.0f || Height <= 0.0f || Type > 4
		|| Trigger.ReTriggerDelay < 0.0f || Trigger.RepeatTriggerTime < 0.0f)
	{
		Error = TEXT("a trigger property is malformed"); return false;
	}
	Trigger.TriggerType = static_cast<EWoTTriggerType>(Type);
	const FString Limited = ClassIdentity.ToString();
	if (Limited.EndsWith(TEXT(".WOTPawn"),ESearchCase::IgnoreCase)
		|| Limited.Equals(TEXT("WOTPawn"),ESearchCase::IgnoreCase))
		Trigger.ClassProximityType = AWoTCharacter::StaticClass();
	else if (Limited.EndsWith(TEXT(".Grunt"),ESearchCase::IgnoreCase)
		|| Limited.Equals(TEXT("Grunt"),ESearchCase::IgnoreCase))
		Trigger.ClassProximityType = AWoTNPCCharacter::StaticClass();
	Trigger.SetUE1CollisionSize(Radius,Height);
	if (!bCollideActors)
	{
		Trigger.Capsule->SetGenerateOverlapEvents(false);
		Trigger.Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	return true;
}

bool ProjectTrigger(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, bool bDoor,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a trigger transform is malformed"); return false; }
	const FName Rule = bDoor ? DoorTriggerRule : TriggerRule;
	AWoTTrigger* Trigger = bDoor
		? static_cast<AWoTTrigger*>(SpawnDeferred<AWoTDoorTrigger>(World,Index,Rule,Transform))
		: SpawnDeferred<AWoTTrigger>(World,Index,Rule,Transform);
	if (!Trigger)
	{ Error=TEXT("a trigger projection could not be allocated"); return false; }
	Trigger->SetReplicates(false);
	if (!ConfigureTrigger(*Trigger,D,A,Error))
	{ Trigger->Destroy(); return false; }
	if (AWoTDoorTrigger* Door = Cast<AWoTDoorTrigger>(Trigger))
	{
		uint8 Direction = 0;
		if (!ReadByte(D,A,TEXT("Direction"),Direction) || Direction > 1)
		{ Error=TEXT("a door trigger direction is malformed"); Trigger->Destroy(); return false; }
		Door->Direction = Direction == 0
			? EWoTDoorDirection::Forward : EWoTDoorDirection::Backward;
	}
	Out.DeferredActors.Add(Trigger); Count(Out,Rule); return true;
}

bool ProjectDispatcher(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a dispatcher transform is malformed"); return false; }
	AWoTDispatcher* Dispatcher=SpawnDeferred<AWoTDispatcher>(
		World,Index,DispatcherRule,Transform);
	if (!Dispatcher)
	{ Error=TEXT("a dispatcher projection could not be allocated"); return false; }
	Dispatcher->SetReplicates(false);
	if (!ReadName(D,A,TEXT("Tag"),Dispatcher->TagName)
		|| !ReadNameSlots(D,A,TEXT("OutEvents"),8,Dispatcher->OutEvents,true)
		|| !ReadFloatSlots(D,A,TEXT("OutDelays"),8,Dispatcher->OutDelays,true))
	{ Error=TEXT("a dispatcher property is malformed"); Dispatcher->Destroy(); return false; }
	Out.DeferredActors.Add(Dispatcher); Count(Out,DispatcherRule); return true;
}

bool ProjectCounter(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a counter transform is malformed"); return false; }
	AWoTCounter* Counter=SpawnDeferred<AWoTCounter>(World,Index,CounterRule,Transform);
	if (!Counter)
	{ Error=TEXT("a counter projection could not be allocated"); return false; }
	Counter->SetReplicates(false);
	if (!ReadName(D,A,TEXT("Tag"),Counter->TagName)
		|| !ReadName(D,A,TEXT("Event"),Counter->EventName)
		|| !ReadInt(D,A,TEXT("NumToCount"),Counter->NumToCount))
	{ Error=TEXT("a counter property is malformed"); Counter->Destroy(); return false; }
	Out.DeferredActors.Add(Counter); Count(Out,CounterRule); return true;
}

const FWoTRuntimeWorldProperty* StructMember(
	const FWoTRuntimeWorldProperty& Property, const TCHAR* Name)
{
	return Property.StructMembers.FindByPredicate(
		[Name](const FWoTRuntimeWorldProperty& Member)
		{ return Member.Name.Equals(Name,ESearchCase::IgnoreCase); });
}

TSubclassOf<AWoTCharacter> ResolveBattleTeamClass(FStringView Identity)
{
	const FString ClassLeaf = Leaf(Identity);
	if (ClassLeaf.Equals(TEXT("AesSedai"), ESearchCase::IgnoreCase))
		return AWoTAesSedai::StaticClass();
	if (ClassLeaf.Equals(TEXT("Forsaken"), ESearchCase::IgnoreCase))
		return AWoTForsaken::StaticClass();
	if (ClassLeaf.Equals(TEXT("Hound"), ESearchCase::IgnoreCase))
		return AWoTHound::StaticClass();
	if (ClassLeaf.Equals(TEXT("Whitecloak"), ESearchCase::IgnoreCase))
		return AWoTWhitecloak::StaticClass();
	return nullptr;
}

bool ReadBattleTeamClasses(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 NumTeams,
	TSubclassOf<AWoTCharacter> (&Out)[4])
{
	for (uint32 Slot = 0; Slot < 4; ++Slot)
	{
		Out[Slot] = nullptr;
		if (static_cast<int32>(Slot) >= NumTeams) continue;
		const FWoTRuntimeWorldProperty* P = Prop(D, A, TEXT("TeamClass"), Slot);
		if (!P || P->Type != EWoTRuntimeWorldPropertyType::Object
			|| P->ObjectIdentity.IsEmpty()) return false;
		Out[Slot] = ResolveBattleTeamClass(P->ObjectIdentity);
		if (!Out[Slot]) return false;
	}
	return true;
}

bool ProjectBattleInfo(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	int32 NumTeams = 0;
	if (!ActorTransform(D, A, Transform)
		|| !ReadInt(D, A, TEXT("NumTeams"), NumTeams)
		|| NumTeams < 2 || NumTeams > 4)
	{
		Error = TEXT("a BattleInfo has malformed team data");
		return false;
	}
	AWoTBattleInfo* Battle = SpawnDeferred<AWoTBattleInfo>(
		World, Index, BattleInfoRule, Transform);
	if (!Battle)
	{
		Error = TEXT("a BattleInfo projection could not be allocated");
		return false;
	}
	Battle->NumTeams = NumTeams;
	if (!ReadBattleTeamClasses(D, A, NumTeams, Battle->TeamClass)
		|| !ReadString(D, A, TEXT("EditingStr"), Battle->EditingStr)
		|| !ReadString(D, A, TEXT("PlayingStr"), Battle->PlayingStr)
		|| !ReadString(D, A, TEXT("ReadyStr"), Battle->ReadyStr)
		|| !ReadString(D, A, TEXT("WaitingStr"), Battle->WaitingStr))
	{
		Error = TEXT("a BattleInfo property is malformed");
		Battle->Destroy();
		return false;
	}
	Battle->SetReplicates(true);
	Battle->SetNetAddressable();
	Out.DeferredActors.Add(Battle);
	Count(Out, BattleInfoRule);
	return true;
}

bool ProjectSealAltar(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	uint8 Team = 255;
	int32 SealGoal = 4;
	float CollisionRadius = 48.0f;
	float CollisionHeight = 1.0f;
	if (!ActorTransform(D, A, Transform)
		|| !ReadByte(D, A, TEXT("Team"), Team)
		|| !ReadInt(D, A, TEXT("SealGoal"), SealGoal)
		|| !ReadFloat(D, A, TEXT("CollisionRadius"), CollisionRadius)
		|| !ReadFloat(D, A, TEXT("CollisionHeight"), CollisionHeight)
		|| SealGoal < 1 || SealGoal > 4
		|| !FMath::IsFinite(CollisionRadius) || CollisionRadius <= 0.0f
		|| !FMath::IsFinite(CollisionHeight) || CollisionHeight < 0.0f)
	{
		Error = TEXT("a SealAltar has malformed properties");
		return false;
	}
	AWoTSealAltar* Altar = SpawnDeferred<AWoTSealAltar>(
		World, Index, SealAltarRule, Transform);
	if (!Altar)
	{
		Error = TEXT("a SealAltar projection could not be allocated");
		return false;
	}
	Altar->SetReplicates(false);
	Altar->Team = Team;
	Altar->SealGoal = SealGoal;
	if (!ReadName(D, A, TEXT("Event"), Altar->EventName)
		|| !ReadName(D, A, TEXT("WinEvent"), Altar->WinEvent)
		|| !ReadString(D, A, TEXT("CaptureNotificationStr"),
			Altar->CaptureNotificationStr)
		|| !IsValid(Altar->Touch))
	{
		Error = TEXT("a SealAltar property is malformed");
		Altar->Destroy();
		return false;
	}
	// Preserve every authored UE1 touch radius and height: the UE capsule is a
	// containing broad phase and the altar filters touches by the exact flat
	// UE1 cylinder (WoTUE1Collision.h).
	Altar->SetUE1CollisionSize(CollisionRadius, CollisionHeight);
	Out.DeferredActors.Add(Altar);
	Count(Out, SealAltarRule);
	return true;
}

bool ReadBudgetResources(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, TArray<FWoTBudgetResource>& Out)
{
	int32 Highest = INDEX_NONE;
	TArray<FWoTBudgetResource> Values;
	Values.SetNum(12);
	for (uint32 Slot = 0; Slot < 12; ++Slot)
	{
		const FWoTRuntimeWorldProperty* P = Prop(D, A, TEXT("Budget"), Slot);
		if (!P) continue;
		Highest = static_cast<int32>(Slot);
		if (P->Type != EWoTRuntimeWorldPropertyType::Struct
			|| !P->StructName.Equals(TEXT("Resource"), ESearchCase::IgnoreCase)
			|| P->StructMembers.Num() != 3) return false;
		const FWoTRuntimeWorldProperty* Package =
			StructMember(*P, TEXT("ResourcePackageName"));
		const FWoTRuntimeWorldProperty* Adapter =
			StructMember(*P, TEXT("ResourceAdapterName"));
		const FWoTRuntimeWorldProperty* CountMember =
			StructMember(*P, TEXT("ItemCount"));
		int32 ItemCount = 0;
		if (!Package || !Adapter || !CountMember
			|| Package->Type != EWoTRuntimeWorldPropertyType::Name
			|| Adapter->Type != EWoTRuntimeWorldPropertyType::Name
			|| !FitsProjectedName(Package->NameValue)
			|| !FitsProjectedName(Adapter->NameValue)
			|| !WoTRuntimeWorldPropertyInteger(*CountMember, ItemCount)
			|| ItemCount < 0) return false;
		Values[Slot].ResourcePackageName = FName(*Package->NameValue);
		Values[Slot].ResourceAdapterName = FName(*Adapter->NameValue);
		Values[Slot].ItemCount = ItemCount;
	}
	if (Highest >= 0)
	{
		Values.SetNum(Highest + 1);
		Out = MoveTemp(Values);
	}
	return true;
}

bool ProjectBudget(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	uint8 Team = 0;
	if (!ActorTransform(D, A, Transform)
		|| !ReadByte(D, A, TEXT("Team"), Team))
	{
		Error = TEXT("a BudgetInfo has malformed properties");
		return false;
	}
	AWoTBudgetInfo* Budget = SpawnDeferred<AWoTBudgetInfo>(
		World, Index, BudgetRule, Transform);
	if (!Budget)
	{
		Error = TEXT("a BudgetInfo projection could not be allocated");
		return false;
	}
	Budget->SetReplicates(false);
	Budget->Team = Team;
	if (!ReadBudgetResources(D, A, Budget->Budget)
		|| Budget->Budget.IsEmpty()
		|| !ReadName(D, A, TEXT("BeginEditEvent"), Budget->BeginEditEvent)
		|| !ReadName(D, A, TEXT("EndEditEvent"), Budget->EndEditEvent))
	{
		Error = TEXT("a BudgetInfo Resource property is malformed");
		Budget->Destroy();
		return false;
	}
	Out.DeferredActors.Add(Budget);
	Count(Out, BudgetRule);
	return true;
}

bool ProjectMashadarGuide(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	uint8 Team = 255;
	float SearchRadius = 600.0f;
	float TriggerRadius = 200.0f;
	if (!ActorTransform(D, A, Transform)
		|| !ReadByte(D, A, TEXT("Team"), Team)
		|| !ReadFloat(D, A, TEXT("SearchRadius"), SearchRadius)
		|| !ReadFloat(D, A, TEXT("TriggerRadius"), TriggerRadius)
		|| !FMath::IsFinite(SearchRadius) || SearchRadius < 0.0f
		|| !FMath::IsFinite(TriggerRadius) || TriggerRadius < 0.0f)
	{
		Error = TEXT("a MashadarGuide has malformed properties");
		return false;
	}
	AWoTMashadarGuide* Guide = SpawnDeferred<AWoTMashadarGuide>(
		World, Index, MashadarGuideRule, Transform);
	if (!Guide)
	{
		Error = TEXT("a MashadarGuide projection could not be allocated");
		return false;
	}
	Guide->Team = Team;
	Guide->SearchRadius = SearchRadius;
	Guide->TriggerRadius = TriggerRadius;
	// Materialization runs before actor initialization on both host and client.
	// Seed every value that simulated BeginPlay restores or uses to place its
	// private chain now: a client startup actor can begin play before its initial
	// replication bunch, so authority-only snapshotting would move it to the CDO
	// origin and permanently build the chain there.
	Guide->InitialGuideLocation = Transform.GetLocation();
	Guide->InitialGuideRotation = Transform.Rotator();
	Guide->HidingPlaceLocation = Transform.GetLocation();
	Guide->InitialSpeed = Guide->Speed;
	Guide->InitialMaxSpeed = Guide->MaxSpeed;
	Guide->InitialAcceleration = Guide->Acceleration;
	// Every map-authored guide is the parked citadel form.
	Guide->bCitadelPlaced = true;
	Guide->SetReplicates(true);
	Guide->SetNetAddressable();
	Out.DeferredActors.Add(Guide);
	Count(Out, MashadarGuideRule);
	return true;
}

bool ReadSoundDispatcherEvents(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, TArray<FWoTSoundDispatcherEvent>& Out)
{
	int32 Highest = INDEX_NONE;
	TArray<FWoTSoundDispatcherEvent> Values;
	Values.SetNum(16);
	for (uint32 Slot=0; Slot<16; ++Slot)
	{
		const FWoTRuntimeWorldProperty* P=Prop(D,A,TEXT("EventList"),Slot);
		if (!P) continue;
		Highest=static_cast<int32>(Slot);
		if (P->Type!=EWoTRuntimeWorldPropertyType::Struct
			|| !P->StructName.Equals(TEXT("SoundEvent"),ESearchCase::IgnoreCase)
			|| P->StructMembers.Num()!=6) return false;
		const FWoTRuntimeWorldProperty* Delay=StructMember(*P,TEXT("Delay"));
		const FWoTRuntimeWorldProperty* Event=StructMember(*P,TEXT("Event"));
		const FWoTRuntimeWorldProperty* Source=StructMember(*P,TEXT("SoundSourceTag"));
		const FWoTRuntimeWorldProperty* Sound=StructMember(*P,TEXT("Sound"));
		const FWoTRuntimeWorldProperty* Table=StructMember(*P,TEXT("SoundTableTag"));
		const FWoTRuntimeWorldProperty* TableSlot=StructMember(*P,TEXT("SoundTableSlot"));
		if (!Delay||!Event||!Source||!Sound||!Table||!TableSlot
			|| !WoTRuntimeWorldPropertyFloat(*Delay,Values[Slot].Delay)
			|| Event->Type!=EWoTRuntimeWorldPropertyType::Name
			|| Source->Type!=EWoTRuntimeWorldPropertyType::Name
			|| Sound->Type!=EWoTRuntimeWorldPropertyType::Object
			|| Table->Type!=EWoTRuntimeWorldPropertyType::Name
			|| TableSlot->Type!=EWoTRuntimeWorldPropertyType::Name) return false;
		Values[Slot].Event=FName(*Event->NameValue);
		Values[Slot].SoundSourceTag=FName(*Source->NameValue);
		Values[Slot].SoundTableTag=FName(*Table->NameValue);
		Values[Slot].SoundTableSlot=FName(*TableSlot->NameValue);
		if (!Sound->ObjectIdentity.IsEmpty())
		{
			FString SubPath;
			if (!RetailObjectSubPath(Sound->ObjectIdentity,
				EWoTNeutralResourceKind::Sound, SubPath)) return false;
			const FString SoundLeaf=Leaf(Sound->ObjectIdentity);
			Values[Slot].Sound=TSoftObjectPtr<USoundBase>(FSoftObjectPath(
				FString::Printf(TEXT("/Game/WoT/Sounds/%s.%s"),
					*SubPath,*SoundLeaf)));
		}
	}
	if (Highest>=0)
	{
		Values.SetNum(Highest+1);
		Out=MoveTemp(Values);
	}
	return true;
}

bool ProjectSoundDispatcher(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a sound dispatcher transform is malformed"); return false; }
	AWoTSoundDispatcher* Dispatcher=SpawnDeferred<AWoTSoundDispatcher>(
		World,Index,SoundDispatcherRule,Transform);
	if (!Dispatcher)
	{ Error=TEXT("a sound dispatcher projection could not be allocated"); return false; }
	Dispatcher->SetReplicates(false);
	if (!ConfigureTrigger(*Dispatcher,D,A,Error)
		|| !ReadSoundDispatcherEvents(D,A,Dispatcher->EventList)
		|| !ReadFloat(D,A,TEXT("SoundVolume"),Dispatcher->SoundVolume)
		|| !ReadFloat(D,A,TEXT("SoundRadius"),Dispatcher->SoundRadius)
		|| !ReadFloat(D,A,TEXT("SoundPitch"),Dispatcher->SoundPitch)
		|| !ReadBool(D,A,TEXT("bSoundOverride"),Dispatcher->bSoundOverride)
		|| !ReadInt(D,A,TEXT("MaxDistance"),Dispatcher->MaxDistance)
		|| !ReadBool(D,A,TEXT("bFromSoundSource"),Dispatcher->bFromSoundSource)
		|| !ReadBool(D,A,TEXT("bTerminate"),Dispatcher->bTerminate))
	{ if(Error.IsEmpty())Error=TEXT("a sound dispatcher property is malformed"); Dispatcher->Destroy(); return false; }
	Out.DeferredActors.Add(Dispatcher); Count(Out,SoundDispatcherRule); return true;
}

bool ProjectPortcullis(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	float Radius=64.0f,Height=160.0f;
	FName Tag,Event;
	if (!ActorTransform(D,A,Transform)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| !ReadName(D,A,TEXT("Tag"),Tag)
		|| !ReadName(D,A,TEXT("Event"),Event)
		|| Radius<=0.0f || Height<=0.0f)
	{ Error=TEXT("a portcullis trigger property is malformed"); return false; }
	AWoTPortcullis* Portcullis=SpawnDeferred<AWoTPortcullis>(
		World,Index,PortcullisRule,Transform);
	if (!Portcullis)
	{ Error=TEXT("a portcullis projection could not be allocated"); return false; }
	Portcullis->SetReplicates(true); Portcullis->SetNetAddressable();
	Portcullis->MoverTag=Event.IsNone()?Tag:Event;
	Portcullis->SetTriggerSize(Radius,Height);
	Out.DeferredActors.Add(Portcullis); Count(Out,PortcullisRule); return true;
}

bool ProjectPitOpening(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	float Radius = 128.0f, Height = 40.0f;
	if (!ActorTransform(D,A,Transform)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| Radius <= 0.0f || Height <= 0.0f)
	{ Error=TEXT("a pit opening property is malformed"); return false; }
	AWoTPitOpening* Opening=SpawnDeferred<AWoTPitOpening>(
		World,Index,PitOpeningRule,Transform);
	if (!Opening)
	{ Error=TEXT("a pit opening projection could not be allocated"); return false; }
	Opening->SetReplicates(true); Opening->SetNetAddressable();
	Opening->SetUE1CollisionSize(Radius,Height);
	Out.DeferredActors.Add(Opening); Count(Out,PitOpeningRule); return true;
}

bool ProjectStaircase(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	float Radius=32.0f,Height=32.0f;
	FName Tag,Event;
	if (!ActorTransform(D,A,Transform)
		|| !ReadName(D,A,TEXT("Tag"),Tag)
		|| !ReadName(D,A,TEXT("Event"),Event)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| Radius<=0.0f || Height<=0.0f)
	{ Error=TEXT("a staircase trigger property is malformed"); return false; }
	AWoTStaircase* Staircase=SpawnDeferred<AWoTStaircase>(
		World,Index,StaircaseRule,Transform);
	if (!Staircase)
	{ Error=TEXT("a staircase projection could not be allocated"); return false; }
	Staircase->SetReplicates(true); Staircase->SetNetAddressable();
	Staircase->MoverTag=Event.IsNone()?Tag:Event;
	Staircase->SetTriggerSize(Radius,Height);
	Out.DeferredActors.Add(Staircase); Count(Out,StaircaseRule); return true;
}

EWoTSpecialEventAction SpecialEventAction(FName State)
{
	if (State==TEXT("DamageInstigator")) return EWoTSpecialEventAction::DamageInstigator;
	if (State==TEXT("KillInstigator")) return EWoTSpecialEventAction::KillInstigator;
	if (State==TEXT("PlaySoundEffect")) return EWoTSpecialEventAction::PlaySoundEffect;
	if (State==TEXT("PlayersPlaySoundEffect")) return EWoTSpecialEventAction::PlayersPlaySoundEffect;
	if (State==TEXT("PlayAmbientSoundEffect")) return EWoTSpecialEventAction::PlayAmbientSoundEffect;
	if (State==TEXT("PlayerPath")) return EWoTSpecialEventAction::PlayerPath;
	return EWoTSpecialEventAction::DisplayMessage;
}

bool ReadSpecialEventConfig(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, FWoTSpecialEventConfig& Config)
{
	FName State(TEXT("DisplayMessage"));
	if (!ReadName(D,A,TEXT("InitialState"),State)
		|| !ReadInt(D,A,TEXT("Damage"),Config.Damage)
		|| !ReadName(D,A,TEXT("DamageType"),Config.DamageType)
		|| !ReadString(D,A,TEXT("Message"),Config.Message)
		|| !ReadBool(D,A,TEXT("bBroadcast"),Config.bBroadcast)
		|| !ReadObjectSubPath(D,A,TEXT("Sound"),Config.SoundSubPath)
		|| !ReadObjectSubPath(D,A,TEXT("AmbientSound"),Config.AmbientSoundSubPath)
		|| !ReadFloat(D,A,TEXT("SoundEffectVolume"),Config.SoundVolume)
		|| !ReadFloat(D,A,TEXT("SoundEffectRadius"),Config.SoundRadius)
		|| !ReadFloat(D,A,TEXT("SoundEffectPitch"),Config.SoundPitch)
		|| Config.SoundVolume < 0.0f || Config.SoundRadius < 0.0f
		|| Config.SoundPitch < 0.0f) return false;
	Config.Action=SpecialEventAction(State);
	return true;
}

bool ProjectSpecialEvent(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, bool bCollisionTrigger,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a special event transform is malformed"); return false; }
	const FName Rule=bCollisionTrigger?SpecialEventTriggerRule:SpecialEventRule;
	if (bCollisionTrigger)
	{
		AWoTSpecialEventTrigger* Trigger=SpawnDeferred<AWoTSpecialEventTrigger>(
			World,Index,Rule,Transform);
		if (!Trigger)
		{ Error=TEXT("a special event trigger could not be allocated"); return false; }
		Trigger->SetReplicates(false);
		if (!ConfigureTrigger(*Trigger,D,A,Error)
			|| !ReadSpecialEventConfig(D,A,Trigger->Config))
		{ if(Error.IsEmpty())Error=TEXT("a special event trigger property is malformed"); Trigger->Destroy(); return false; }
		Out.DeferredActors.Add(Trigger);
	}
	else
	{
		AWoTSpecialEvent* Event=SpawnDeferred<AWoTSpecialEvent>(World,Index,Rule,Transform);
		if (!Event)
		{ Error=TEXT("a special event could not be allocated"); return false; }
		Event->SetReplicates(false);
		if (!ReadName(D,A,TEXT("Tag"),Event->TagName)
			|| !ReadName(D,A,TEXT("Event"),Event->EventName)
			|| !ReadSpecialEventConfig(D,A,Event->Config))
		{ Error=TEXT("a special event property is malformed"); Event->Destroy(); return false; }
		Out.DeferredActors.Add(Event);
	}
	Count(Out,Rule); return true;
}

bool ProjectRandomEventTrigger(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, bool bLocalOnly,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	FName State(TEXT("TriggeredActive"));
	const FName Rule=bLocalOnly?SpecialEffectsTriggerRule:StochasticTriggerRule;
	AWoTRandomEventTrigger* Trigger=nullptr;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a random event trigger transform is malformed"); return false; }
	Trigger=SpawnDeferred<AWoTRandomEventTrigger>(World,Index,Rule,Transform);
	if (!Trigger)
	{ Error=TEXT("a random event trigger could not be allocated"); return false; }
	Trigger->SetReplicates(false); Trigger->bLocalOnly=bLocalOnly;
	if (!ReadName(D,A,TEXT("Tag"),Trigger->TagName)
		|| !ReadName(D,A,TEXT("InitialState"),State)
		|| !ReadNameSlots(D,A,TEXT("Events"),6,Trigger->Events,true)
		|| !ReadFloat(D,A,TEXT("triggerProbability"),Trigger->TriggerProbability)
		|| !ReadFloat(D,A,TEXT("minReCheckTime"),Trigger->MinRecheckTime)
		|| !ReadFloat(D,A,TEXT("maxReCheckTime"),Trigger->MaxRecheckTime)
		|| Trigger->TriggerProbability<0.0f || Trigger->TriggerProbability>1.0f
		|| Trigger->MinRecheckTime<0.0f || Trigger->MaxRecheckTime<Trigger->MinRecheckTime)
	{ Error=TEXT("a random event trigger property is malformed"); Trigger->Destroy(); return false; }
	Trigger->Events.RemoveAll([](FName Event){return Event.IsNone();});
	Trigger->bInitiallyActive=State==TEXT("AlwaysActive");
	Out.DeferredActors.Add(Trigger); Count(Out,Rule); return true;
}

bool ProjectTriggerLight(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	FName State(TEXT("TriggerControl"));
	uint8 Brightness=64,Hue=0,Saturation=255,Radius=64;
	AWoTTriggerLight* Light=nullptr;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a trigger light transform is malformed"); return false; }
	Light=SpawnDeferred<AWoTTriggerLight>(World,Index,TriggerLightRule,Transform);
	if (!Light)
	{ Error=TEXT("a trigger light could not be allocated"); return false; }
	Light->SetReplicates(true); Light->SetNetAddressable();
	if (!ReadName(D,A,TEXT("Tag"),Light->TagName)
		|| !ReadName(D,A,TEXT("InitialState"),State)
		|| !ReadFloat(D,A,TEXT("ChangeTime"),Light->ChangeTime)
		|| !ReadBool(D,A,TEXT("bInitiallyOn"),Light->bInitiallyOn)
		|| !ReadBool(D,A,TEXT("bDelayFullOn"),Light->bDelayFullOn)
		|| !ReadFloat(D,A,TEXT("RemainOnTime"),Light->RemainOnTime)
		|| !ReadByte(D,A,TEXT("LightBrightness"),Brightness)
		|| !ReadByte(D,A,TEXT("LightHue"),Hue)
		|| !ReadByte(D,A,TEXT("LightSaturation"),Saturation)
		|| !ReadByte(D,A,TEXT("LightRadius"),Radius)
		|| Light->ChangeTime<0.0f || Light->RemainOnTime<0.0f)
	{ Error=TEXT("a trigger light property is malformed"); Light->Destroy(); return false; }
	if (State==TEXT("TriggerTurnsOn")) Light->Mode=EWoTTriggerLightMode::TurnsOn;
	else if (State==TEXT("TriggerTurnsOff")) Light->Mode=EWoTTriggerLightMode::TurnsOff;
	else if (State==TEXT("TriggerToggle")) Light->Mode=EWoTTriggerLightMode::Toggle;
	else if (State==TEXT("TriggerPound")) Light->Mode=EWoTTriggerLightMode::Pound;
	else Light->Mode=EWoTTriggerLightMode::Control;
	Light->FullIntensity=FMath::Pow(Brightness/255.0f,2.2f)*5.0f;
	Light->Light->SetLightFColor(ImporterPointLightColor(Hue,Saturation));
	Light->Light->SetAttenuationRadius(FMath::Max(1.0f,Radius*25.0f));
	Light->Light->SetLightFalloffExponent(5.3f);
	Light->Light->LightingChannels.bChannel0=false;
	Light->Light->LightingChannels.bChannel1=true;
	Light->Light->LightingChannels.bChannel2=false;
	Out.DeferredActors.Add(Light); Count(Out,TriggerLightRule); return true;
}

bool ProjectTeleporter(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	float Radius=18.0f,Height=40.0f;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a teleporter transform is malformed"); return false; }
	AWoTTeleporter* Teleporter=SpawnDeferred<AWoTTeleporter>(
		World,Index,TeleporterRule,Transform);
	if (!Teleporter)
	{ Error=TEXT("a teleporter projection could not be allocated"); return false; }
	Teleporter->SetReplicates(true); Teleporter->SetNetAddressable();
	Teleporter->bMakesTeleportNoise = !Leaf(A.ClassIdentity).Equals(
		TEXT("TeleporterWithEffects"), ESearchCase::IgnoreCase);
	if (!ReadString(D,A,TEXT("URL"),Teleporter->URL)
		|| !ReadName(D,A,TEXT("Tag"),Teleporter->TeleTag)
		|| !ReadName(D,A,TEXT("Event"),Teleporter->EventName)
		|| !ReadBool(D,A,TEXT("bEnabled"),Teleporter->bEnabled)
		|| !ReadBool(D,A,TEXT("bChangesVelocity"),Teleporter->bChangesVelocity)
		|| !ReadBool(D,A,TEXT("bChangesYaw"),Teleporter->bChangesYaw)
		|| !ReadBool(D,A,TEXT("bReversesX"),Teleporter->bReversesX)
		|| !ReadBool(D,A,TEXT("bReversesY"),Teleporter->bReversesY)
		|| !ReadBool(D,A,TEXT("bReversesZ"),Teleporter->bReversesZ)
		|| !ReadVector(D,A,TEXT("TargetVelocity"),Teleporter->TargetVelocity)
		|| !ReadObjectLeafName(D,A,TEXT("EnterEffectClass"),Teleporter->EnterEffectConfig)
		|| !ReadObjectLeafName(D,A,TEXT("ExitEffectClass"),Teleporter->ExitEffectConfig)
		|| !ReadObjectSubPath(D,A,TEXT("EnterEffectSound"),Teleporter->EnterEffectSoundPath)
		|| !ReadObjectSubPath(D,A,TEXT("ExitEffectSound"),Teleporter->ExitEffectSoundPath)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| Radius<=0.0f || Height<=0.0f)
	{ Error=TEXT("a teleporter property is malformed"); Teleporter->Destroy(); return false; }
	Teleporter->SetUE1CollisionSize(Radius,Height);
	Out.DeferredActors.Add(Teleporter); Count(Out,TeleporterRule); return true;
}

bool ProjectJumper(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	float Radius=40.0f,Height=40.0f;
	if (!ActorTransform(D,A,Transform))
	{ Error=TEXT("a jumper transform is malformed"); return false; }
	AWoTJumper* Jumper=SpawnDeferred<AWoTJumper>(World,Index,JumperRule,Transform);
	if (!Jumper)
	{ Error=TEXT("a jumper projection could not be allocated"); return false; }
	Jumper->SetReplicates(false);
	if (!ReadBool(D,A,TEXT("bOnceOnly"),Jumper->bOnceOnly)
		|| !ReadFloat(D,A,TEXT("JumpZ"),Jumper->JumpZ)
		|| !ReadObjectIdentity(D,A,TEXT("LimitedToClass"),Jumper->LimitedToClassIdentity)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| Radius<=0.0f || Height<=0.0f)
	{ Error=TEXT("a jumper property is malformed"); Jumper->Destroy(); return false; }
	Jumper->SetUE1CollisionSize(Radius,Height);
	Out.DeferredActors.Add(Jumper); Count(Out,JumperRule); return true;
}

bool ProjectBlocker(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, EWoTCollisionBlockerMode Mode,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	float Radius=40.0f,Height=40.0f;
	if (!ActorTransform(D,A,Transform)
		|| !ReadFloat(D,A,TEXT("CollisionRadius"),Radius)
		|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
		|| Radius<=0.0f || Height<=0.0f)
	{ Error=TEXT("a collision blocker property is malformed"); return false; }
	const FName Rule=Mode==EWoTCollisionBlockerMode::Players?BlockPlayerRule:
		Mode==EWoTCollisionBlockerMode::Monsters?BlockMonstersRule:BlockAllRule;
	AWoTCollisionBlocker* Blocker=SpawnDeferred<AWoTCollisionBlocker>(
		World,Index,Rule,Transform);
	if (!Blocker)
	{ Error=TEXT("a collision blocker projection could not be allocated"); return false; }
	Blocker->SetReplicates(false); Blocker->Configure(Mode,Radius,Height);
	Out.DeferredActors.Add(Blocker); Count(Out,Rule); return true;
}

void ProjectSoundMarkers(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D, FWoTWorldProjectionResult& Out)
{
	TSet<FName> Placed;
	for (int32 DispatcherIndex=0; DispatcherIndex<Source.Records.Num(); ++DispatcherIndex)
	{
		const FWoTRuntimeWorldActor& Dispatcher=Source.Records[DispatcherIndex];
		if (!Leaf(Dispatcher.ClassIdentity).Equals(
			TEXT("SoundDispatcher"),ESearchCase::IgnoreCase)) continue;
		TArray<FWoTSoundDispatcherEvent> Events;
		if (!ReadSoundDispatcherEvents(D,Dispatcher,Events)) continue;
		int32 Suffix=0;
		for (const FWoTSoundDispatcherEvent& Event:Events)
		{
			const FName Wanted=Event.SoundSourceTag;
			if (Wanted.IsNone() || Placed.Contains(Wanted)) continue;
			int32 Matches=0,SourceIndex=INDEX_NONE;
			for (int32 Index=0;Index<Source.Records.Num();++Index)
			{
				FName Tag;
				if (ReadName(D,Source.Records[Index],TEXT("Tag"),Tag)
					&& Tag==Wanted) { ++Matches; if(SourceIndex==INDEX_NONE)SourceIndex=Index; }
			}
			bool bRegistrySource=false;
			if (Matches==1)
			{
				const FString SourceLeaf=Leaf(Source.Records[SourceIndex].ClassIdentity);
				bRegistrySource=SourceLeaf.Equals(TEXT("Trigger"),ESearchCase::IgnoreCase)
					|| SourceLeaf.Equals(TEXT("SoundDispatcher"),ESearchCase::IgnoreCase);
			}
			if (bRegistrySource) continue;
			FTransform Transform;
			const FWoTRuntimeWorldActor& Placement=SourceIndex==INDEX_NONE
				? Dispatcher:Source.Records[SourceIndex];
			if (!ActorTransform(D,Placement,Transform)) continue;
			AWoTTaggedMarker* Marker=SpawnDeferred<AWoTTaggedMarker>(
				World,DispatcherIndex,TaggedMarkerRule,Transform,Suffix++);
			if (!Marker) continue;
			Marker->SetReplicates(false); Marker->TagName=Wanted;
			Out.DeferredActors.Add(Marker); Count(Out,TaggedMarkerRule); Placed.Add(Wanted);
		}
	}
}

bool ProjectLeafMap(UWorld& World, const FWoTWorldProjectionSource& Source,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	AWoTLeafMap* LeafMap = SpawnDeferred<AWoTLeafMap>(
		World, 0, LeafMapRule, FTransform::Identity);
	if (!LeafMap)
	{
		Error = TEXT("the runtime leaf map could not be allocated");
		return false;
	}
	LeafMap->SetReplicates(false);
	LeafMap->NodePlanes.Reserve(Source.Nodes.Num() * 4);
	LeafMap->NodeChildren.Reserve(Source.Nodes.Num() * 2);
	LeafMap->NodeLeaves.Reserve(Source.Nodes.Num() * 2);
	LeafMap->NodeCsg.Reserve(Source.Nodes.Num());
	for (const FWoTRuntimeWorldNode& Node : Source.Nodes)
	{
		for (float Value : Node.Plane) LeafMap->NodePlanes.Add(Value);
		LeafMap->NodeChildren.Add(Node.Back);
		LeafMap->NodeChildren.Add(Node.Front);
		LeafMap->NodeLeaves.Add(Node.Leaves[0]);
		LeafMap->NodeLeaves.Add(Node.Leaves[1]);
		constexpr uint8 NFNotCsg = 0x01;
		constexpr uint8 NFIsNew = 0x20;
		LeafMap->NodeCsg.Add(Node.VertexCount > 0
			&& (Node.Flags & (NFNotCsg | NFIsNew)) == 0 ? 1 : 0);
	}
	LeafMap->bRootOutside = Source.bBSPRootOutside;
	LeafMap->NumLeaves = Source.Leaves.Num();
	Out.DeferredActors.Add(LeafMap);
	Count(Out, LeafMapRule);
	return true;
}

bool ProjectAmbientSound(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FName Sound;
	if (!ReadObjectSubPath(D, A, TEXT("AmbientSound"), Sound))
	{
		Error = TEXT("an ambient sound identity is malformed");
		return false;
	}
	if (Sound.IsNone()) return true;
	FTransform Transform;
	uint8 Radius = 32, Volume = 128, Pitch = 64;
	uint8 LightType = 0, LightBrightness = 0, LightPeriod = 0, LightPhase = 0;
	if (!ActorTransform(D, A, Transform)
		|| !ReadByte(D, A, TEXT("SoundRadius"), Radius)
		|| !ReadByte(D, A, TEXT("SoundVolume"), Volume)
		|| !ReadByte(D, A, TEXT("SoundPitch"), Pitch)
		|| !ReadByte(D, A, TEXT("LightType"), LightType)
		|| !ReadByte(D, A, TEXT("LightBrightness"), LightBrightness)
		|| !ReadByte(D, A, TEXT("LightPeriod"), LightPeriod)
		|| !ReadByte(D, A, TEXT("LightPhase"), LightPhase))
	{
		Error = TEXT("an ambient sound property is malformed");
		return false;
	}
	AWoTAmbientSound* Ambient = SpawnDeferred<AWoTAmbientSound>(
		World, Index, AmbientSoundRule, Transform);
	if (!Ambient)
	{
		Error = TEXT("an ambient sound projection could not be allocated");
		return false;
	}
	Ambient->SetReplicates(false);
	Ambient->SoundSubPath = Sound;
	Ambient->SoundRadius = Radius;
	Ambient->SoundVolume = Volume;
	Ambient->SoundPitch = Pitch;
	Ambient->LightType = LightType;
	Ambient->LightBrightness = LightBrightness;
	Ambient->LightPeriod = LightPeriod;
	Ambient->LightPhase = LightPhase;
	Out.DeferredActors.Add(Ambient);
	Count(Out, AmbientSoundRule);
	return true;
}

bool ProjectDynamicAmbientSound(UWorld& World,
	const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A,
	int32 Index, FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	bool InitiallyOn = false, DontRepeat = false;
	float Probability = 0.6f, MinTime = 5.0f, MaxTime = 10.0f;
	uint8 Radius = 32, Volume = 42;
	TArray<FName> Sounds;
	if (!ActorTransform(D, A, Transform)
		|| !ReadBool(D, A, TEXT("bInitiallyOn"), InitiallyOn)
		|| !ReadBool(D, A, TEXT("bDontRepeat"), DontRepeat)
		|| !ReadFloat(D, A, TEXT("PlayProbability"), Probability)
		|| !ReadFloat(D, A, TEXT("MinReCheckTime"), MinTime)
		|| !ReadFloat(D, A, TEXT("MaxReCheckTime"), MaxTime)
		|| !ReadByte(D, A, TEXT("SoundRadius"), Radius)
		|| !ReadByte(D, A, TEXT("SoundVolume"), Volume)
		|| !ReadNameSlots(D, A, TEXT("Sounds"), 6, Sounds, true, true))
	{
		Error = TEXT("a dynamic ambient sound property is malformed");
		return false;
	}
	AWoTDynamicAmbientSound* Dynamic = SpawnDeferred<AWoTDynamicAmbientSound>(
		World, Index, DynamicAmbientSoundRule, Transform);
	if (!Dynamic)
	{
		Error = TEXT("a dynamic ambient sound projection could not be allocated");
		return false;
	}
	Dynamic->SetReplicates(false);
	Dynamic->bInitiallyOn = InitiallyOn;
	Dynamic->bDontRepeat = DontRepeat;
	Dynamic->PlayProbability = Probability;
	Dynamic->MinReCheckTime = MinTime;
	Dynamic->MaxReCheckTime = MaxTime;
	Dynamic->SoundRadius = Radius;
	Dynamic->SoundVolume = Volume;
	Dynamic->SoundSubPaths = MoveTemp(Sounds);
	Out.DeferredActors.Add(Dynamic);
	Count(Out, DynamicAmbientSoundRule);
	return true;
}

bool ProjectCorona(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	const TSet<FName>& LightningTags,
	const TMap<FString, TArray<int32>>& LeavesByIdentity,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	bool bCorona = false;
	if (!ReadBool(D, A, TEXT("bCorona"), bCorona))
	{
		Error = TEXT("a corona flag is malformed");
		return false;
	}
	if (!bCorona) return true;
	FName Skin;
	FName Tag;
	if (!ReadObjectSubPath(D, A, TEXT("Skin"), Skin, EWoTNeutralResourceKind::Texture)
		|| !ReadName(D, A, TEXT("Tag"), Tag))
	{
		Error = TEXT("a corona skin or tag is malformed");
		return false;
	}
	if (Skin.IsNone() || LightningTags.Contains(Tag)) return true;
	FTransform Transform;
	float DrawScale = 1.0f;
	uint8 Radius = 64, VolumeRadius = 0, Hue = 0, Saturation = 255;
	bool bStatic = true;
	if (!ActorTransform(D, A, Transform)
		|| !ReadFloat(D, A, TEXT("DrawScale"), DrawScale)
		|| !ReadByte(D, A, TEXT("LightRadius"), Radius)
		|| !ReadByte(D, A, TEXT("VolumeRadius"), VolumeRadius)
		|| !ReadByte(D, A, TEXT("LightHue"), Hue)
		|| !ReadByte(D, A, TEXT("LightSaturation"), Saturation)
		|| !ReadBool(D, A, TEXT("bStatic"), bStatic))
	{
		Error = TEXT("a corona property is malformed");
		return false;
	}
	AWoTCorona* Corona = SpawnDeferred<AWoTCorona>(
		World, Index, CoronaRule, Transform);
	if (!Corona)
	{
		Error = TEXT("a corona projection could not be allocated");
		return false;
	}
	Corona->SetReplicates(false);
	Corona->DrawScale = DrawScale;
	Corona->Reach = 25.0f * (static_cast<float>(Radius) + 1.0f);
	if (VolumeRadius > 0)
		Corona->Reach = FMath::Max(Corona->Reach,
			25.0f * (static_cast<float>(VolumeRadius) + 1.0f));
	FVector3f HueColor;
	if (Hue < 86)
		HueColor = FVector3f((85.0f - Hue) / 85.0f, Hue / 85.0f, 0.0f);
	else if (Hue < 171)
		HueColor = FVector3f(0.0f, (170.0f - Hue) / 85.0f,
			(Hue - 85.0f) / 85.0f);
	else
		HueColor = FVector3f((Hue - 170.0f) / 85.0f, 0.0f,
			(255.0f - Hue) / 84.0f);
	const float Alpha = static_cast<float>(Saturation) / 255.0f;
	HueColor.X += Alpha * (1.0f - HueColor.X);
	HueColor.Y += Alpha * (1.0f - HueColor.Y);
	HueColor.Z += Alpha * (1.0f - HueColor.Z);
	Corona->Color = FLinearColor(HueColor.X, HueColor.Y, HueColor.Z, 1.0f);
	Corona->SkinSubPath = Skin;
	Corona->bLeafGated = bStatic;
	if (const TArray<int32>* Leaves = LeavesByIdentity.Find(A.Identity.ToLower()))
		Corona->PermeatingLeaves = *Leaves;
	Corona->PermeatingLeaves.Sort();
	Out.DeferredActors.Add(Corona);
	Count(Out, CoronaRule);
	return true;
}

bool ProjectRainCyd(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform Transform;
	FWoTRuntimeActorVisualPresentation Visual;
	if (!ActorTransform(D, A, Transform)
		|| !WoTRuntimeReadActorVisualPresentation(D, A, Visual, Error)
		|| !Visual.HasMesh())
	{
		if (Error.IsEmpty()) Error = TEXT("RainCyd has no effective retail mesh");
		return false;
	}
	float CylinderHeight = 48.0f, MinFall = 500.0f, MaxFall = 700.0f;
	float TraceDepth = 1000.0f, BuryDistance = 100.0f;
	float VisibilityRadius = 0.0f, VisibilityHeight = 0.0f;
	int32 StackNum = 5;
	if (!ReadFloat(D, A, TEXT("CylinderHeight"), CylinderHeight)
		|| !ReadInt(D, A, TEXT("StackNum"), StackNum)
		|| !ReadFloat(D, A, TEXT("MinFallRate"), MinFall)
		|| !ReadFloat(D, A, TEXT("MaxFallRate"), MaxFall)
		|| !ReadFloat(D, A, TEXT("TraceDepth"), TraceDepth)
		|| !ReadFloat(D, A, TEXT("BuryDistance"), BuryDistance)
		|| !ReadFloat(D, A, TEXT("VisibilityRadius"), VisibilityRadius)
		|| !ReadFloat(D, A, TEXT("VisibilityHeight"), VisibilityHeight)
		|| StackNum < 1 || StackNum > 64 || CylinderHeight <= 0.0f
		|| MinFall < 0.0f || MaxFall < MinFall || TraceDepth < 0.0f
		|| VisibilityRadius < 0.0f || VisibilityHeight < 0.0f)
	{
		Error = TEXT("a RainCyd property is malformed");
		return false;
	}
	AWoTRainCyd* Rain = SpawnDeferred<AWoTRainCyd>(
		World, Index, RainCydRule, Transform);
	if (!Rain)
	{
		Error = TEXT("a RainCyd projection could not be allocated");
		return false;
	}
	Rain->SetReplicates(false);
	Rain->CylinderHeight = CylinderHeight;
	Rain->StackNum = StackNum;
	Rain->MinFallRate = MinFall;
	Rain->MaxFallRate = MaxFall;
	Rain->TraceDepth = TraceDepth;
	Rain->BuryDistance = BuryDistance;
	Rain->VisibilityRadius = VisibilityRadius;
	Rain->VisibilityHeight = VisibilityHeight;
	Rain->SetActorScale3D(FVector(Visual.DrawScale));
	Rain->ConfigureRuntimePresentation(Visual.MeshName, Visual.MeshPackage,
		Visual.Skin, Visual.SkinOverrides, Visual.AmbientGlow, Visual.ScaleGlow);
	Out.DeferredActors.Add(Rain);
	Count(Out, RainCydRule);
	return true;
}


// ---------------------------------------------------------------------------
// Fog lights: Tools/convert/foglights.py extract() + import_map.py
// place_foglights. The set is the union of (1) every light referenced by any
// BSP leaf's baked iVolumetric run and (2) every actor whose INSTANCE
// VolumeRadius is non-zero and whose location's BSP zone is a bFogZone zone.
// Per light: radius 25*(VolumeRadius+1); color hsv(Hue,Sat,255)*Brightness/255
// *LevelInfo.Brightness*type-wave-mean; VolB = mean*VolumeBrightness/64.
// Lights with LT_None, zero brightness, or zero VolumeBrightness emit nothing.
// ---------------------------------------------------------------------------

/** UE1 BSP point-to-zone descent (lightmaps.py point_zone): front when the
    point is on or in front of the node plane; the leaf side's iZone. */
int32 FogLightPointZone(const FWoTWorldProjectionSource& Source, const FVector& Point)
{
	if (Source.Nodes.IsEmpty() || Source.Zones.IsEmpty()) return 0;
	int32 Node = 0;
	int32 Zone = 0;
	for (int32 Depth = 0; Node != INDEX_NONE && Depth <= Source.Nodes.Num(); ++Depth)
	{
		if (!Source.Nodes.IsValidIndex(Node)) return 0;
		const FWoTRuntimeWorldNode& N = Source.Nodes[Node];
		const double Distance = Point.X * static_cast<double>(N.Plane[0])
			+ Point.Y * static_cast<double>(N.Plane[1])
			+ Point.Z * static_cast<double>(N.Plane[2])
			- static_cast<double>(N.Plane[3]);
		const bool bFront = Distance >= 0.0;
		Zone = N.Zones[bFront ? 1 : 0];
		Node = bFront ? N.Front : N.Back;
	}
	return Zone;
}

bool ProjectFogLights(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	TMap<FString, int32> RecordByIdentity;
	for (int32 Index = 0; Index < Source.Records.Num(); ++Index)
		RecordByIdentity.Add(Source.Records[Index].Identity.ToLower(), Index);
	// (1) the editor-baked volumetric set: every leaf's iVolumetric run.
	TSet<int32> Selected;
	for (const FWoTRuntimeWorldLeaf& BSPLeaf : Source.Leaves)
	{
		if (BSPLeaf.Volumetric < 0) continue;
		for (int32 LightIndex = BSPLeaf.Volumetric;
			Source.ModelLights.IsValidIndex(LightIndex)
				&& Source.ModelLights[LightIndex] != MAX_uint32; ++LightIndex)
		{
			const uint32 SemanticIndex = Source.ModelLights[LightIndex];
			if (!Source.SemanticObjects.IsValidIndex(static_cast<int32>(SemanticIndex)))
			{
				Error = TEXT("a leaf volumetric light is outside the semantic registry");
				return false;
			}
			const FWoTRuntimeWorldSemanticObject& Semantic =
				Source.SemanticObjects[static_cast<int32>(SemanticIndex)];
			if (!Semantic.bActor) continue;
			if (const int32* Record = RecordByIdentity.Find(Semantic.Identity.ToLower()))
				Selected.Add(*Record);
		}
	}
	// (2) the renderer's dynamic rule: the actor's own zone is a fog zone.
	TSet<FString> FogZoneActors;
	for (const FWoTRuntimeWorldZone& Zone : Source.Zones)
	{
		const int32* Record = RecordByIdentity.Find(Zone.ActorIdentity.ToLower());
		if (Record == nullptr) continue;
		bool bFogZone = false;
		if (!ReadBool(D, Source.Records[*Record], TEXT("bFogZone"), bFogZone))
		{
			// The Zone rule owns that actor's report; fog projection treats
			// the malformed zone as non-fog and keeps the world.
			SkipActor(Out, FogLightRule, Source.Records[*Record].Identity,
				TEXT("a zone fog flag is malformed"));
			continue;
		}
		if (bFogZone) FogZoneActors.Add(Zone.ActorIdentity.ToLower());
	}
	if (!FogZoneActors.IsEmpty())
	{
		for (int32 Index = 0; Index < Source.Records.Num(); ++Index)
		{
			if (Selected.Contains(Index)) continue;
			const FWoTRuntimeWorldActor& A = Source.Records[Index];
			uint8 InstanceVolumeRadius = 0;
			FVector Location = FVector::ZeroVector;
			if (!ReadInstanceByte(A, TEXT("VolumeRadius"), InstanceVolumeRadius)
				|| !ReadInstanceVector(A, TEXT("Location"), Location)) continue;
			if (InstanceVolumeRadius == 0) continue;
			const int32 Zone = FogLightPointZone(Source, Location);
			if (!Source.Zones.IsValidIndex(Zone)) continue;
			if (FogZoneActors.Contains(Source.Zones[Zone].ActorIdentity.ToLower()))
				Selected.Add(Index);
		}
	}
	if (Selected.IsEmpty()) return true;
	float LevelBrightness = 1.0f;
	for (const FWoTRuntimeWorldActor& A : Source.Records)
	{
		if (!Leaf(A.ClassIdentity).Equals(TEXT("LevelInfo"), ESearchCase::IgnoreCase))
			continue;
		if (!ReadFloat(D, A, TEXT("Brightness"), LevelBrightness))
		{
			// Every fog light scales by the level brightness the LightTable
			// rule already reported as malformed: skip the rule, keep the world.
			SkipActor(Out, FogLightRule, A.Identity,
				TEXT("the LevelInfo brightness is malformed"));
			return true;
		}
		break;
	}
	TArray<int32> Ordered = Selected.Array();
	Ordered.Sort();
	for (const int32 Index : Ordered)
	{
		const FWoTRuntimeWorldActor& A = Source.Records[Index];
		uint8 Type = 0, Brightness = 0, Hue = 0, Saturation = 0;
		uint8 VolumeBrightness = 0, VolumeRadius = 0;
		FTransform Transform;
		if (!ReadByte(D, A, TEXT("LightType"), Type)
			|| !ReadByte(D, A, TEXT("LightBrightness"), Brightness)
			|| !ReadByte(D, A, TEXT("LightHue"), Hue)
			|| !ReadByte(D, A, TEXT("LightSaturation"), Saturation)
			|| !ReadByte(D, A, TEXT("VolumeBrightness"), VolumeBrightness)
			|| !ReadByte(D, A, TEXT("VolumeRadius"), VolumeRadius)
			|| !ActorTransform(D, A, Transform))
		{
			SkipActor(Out, FogLightRule, A.Identity, TEXT("a fog light property is malformed"));
			continue;
		}
		if (VolumeBrightness == 0 || Type == 0 || Brightness == 0) continue;
		const double Mean = UWoTDynamicLightSubsystem::RetailWaveMean(Type);
		const FVector3d HSV = UE1FGetHSVDouble(Hue, Saturation, 255);
		const double Scale = (static_cast<double>(Brightness) / 255.0)
			* static_cast<double>(LevelBrightness) * Mean;
		FVector Location = Transform.GetLocation();
		Location.X = RuntimeProjectedLocation(Location.X);
		Location.Y = RuntimeProjectedLocation(Location.Y);
		Location.Z = RuntimeProjectedLocation(Location.Z);
		AWoTFogLight* Fog = SpawnDeferred<AWoTFogLight>(
			World, Index, FogLightRule, FTransform(FRotator::ZeroRotator, Location));
		if (!Fog)
		{
			Error = TEXT("a fog light projection could not be allocated");
			return false;
		}
		Fog->SetReplicates(false);
		Fog->Radius = 25.0f * (static_cast<float>(VolumeRadius) + 1.0f);
		Fog->FogColor = FLinearColor(
			static_cast<float>(HSV.X * Scale), static_cast<float>(HSV.Y * Scale),
			static_cast<float>(HSV.Z * Scale), 1.0f);
		Fog->VolBrightness = static_cast<float>(
			Mean * static_cast<double>(VolumeBrightness) / 64.0);
		Fog->bRuntimeProjected = true;
		Out.DeferredActors.Add(Fog);
		Count(Out, FogLightRule);
	}
	return true;
}


// ---------------------------------------------------------------------------
// Light wave driver: import_map.py place_lightwave from the atlas animation
// manifest (Tools/convert/lightmaps.py animation_manifest). One AWoTLightWave
// per map whose Groups are the sorted exact (LightType, LightPeriod,
// LightPhase) waveform keys of every emitting Pulse/SubtlePulse light; a
// shimmer-only map still gets the driver (empty Groups) so NoiseFrame
// advances; a map with no animated light gets no actor. The runtime BSP
// carries the same manifest in its version-5 trailer; the census proves both
// agree.
// ---------------------------------------------------------------------------
bool ProjectLightWave(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D, const TSet<FName>& LightningTags,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	struct FWaveKey
	{
		uint8 Type = 0;
		uint8 Period = 0;
		uint8 Phase = 0;
	};
	TArray<FWaveKey> Groups;
	bool bShimmer = false;
	for (const FWoTRuntimeWorldActor& A : Source.Records)
	{
		uint8 Type = 0, Effect = 0, Brightness = 0, Period = 0, Phase = 0;
		FName Tag;
		// The Light rule owns the report for a malformed LightType,
		// LightBrightness, or Tag (ProjectLight skips that actor); the wave
		// rule reports only its own animation properties and keeps the world.
		if (!ReadByte(D, A, TEXT("LightType"), Type)
			|| !ReadByte(D, A, TEXT("LightBrightness"), Brightness)
			|| !ReadName(D, A, TEXT("Tag"), Tag)) continue;
		if (!ReadByte(D, A, TEXT("LightEffect"), Effect)
			|| !ReadByte(D, A, TEXT("LightPeriod"), Period)
			|| !ReadByte(D, A, TEXT("LightPhase"), Phase))
		{
			SkipActor(Out, LightWaveRule, A.Identity,
				TEXT("a light animation property is malformed"));
			continue;
		}
		// lightmaps.py: LIGHT_TYPE_MEAN (Steady/Pulse/SubtlePulse) and
		// SUPPORTED_EFFECTS; Lightning-folded actors never animate.
		const bool bSupportedType = Type == 1 || Type == 2 || Type == 3 || Type == 7;
		const bool bSupportedEffect = Effect == 0 || Effect == 1 || Effect == 2
			|| Effect == 3 || Effect == 8 || Effect == 12 || Effect == 13 || Effect == 17;
		if (Brightness == 0 || !bSupportedType || !bSupportedEffect
			|| LightningTags.Contains(Tag)) continue;
		if (Effect >= 1 && Effect <= 3) bShimmer = true;
		if (Type != 2 && Type != 3 && Type != 7) continue;
		bool bFound = false;
		for (const FWaveKey& Key : Groups)
			if (Key.Type == Type && Key.Period == Period && Key.Phase == Phase)
			{
				bFound = true;
				break;
			}
		if (!bFound) Groups.Add({Type, Period, Phase});
	}
	if (Groups.IsEmpty() && !bShimmer) return true;
	Groups.Sort([](const FWaveKey& L, const FWaveKey& R)
	{
		if (L.Type != R.Type) return L.Type < R.Type;
		if (L.Period != R.Period) return L.Period < R.Period;
		return L.Phase < R.Phase;
	});
	if (Groups.Num() > AWoTLightWave::MaxWaveformGroups)
	{
		Error = TEXT("the map exceeds four waveform groups");
		return false;
	}
	AWoTLightWave* Wave = SpawnDeferred<AWoTLightWave>(
		World, 0, LightWaveRule, FTransform::Identity);
	if (!Wave)
	{
		Error = TEXT("the light wave driver could not be allocated");
		return false;
	}
	Wave->SetReplicates(false);
	for (const FWaveKey& Key : Groups)
	{
		FWoTLightWaveGroup& Group = Wave->Groups.AddDefaulted_GetRef();
		Group.Type = Key.Type;
		Group.Period = Key.Period;
		Group.Phase = Key.Phase;
	}
	Out.DeferredActors.Add(Wave);
	Count(Out, LightWaveRule);
	return true;
}

bool ReadRotatorOffsetUnits(const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, FRotator& Out)
{
	const FWoTRuntimeWorldProperty* Property =
		Prop(D, A, TEXT("ActorRotationOffset"));
	if (!Property) return true;
	if (Property->Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property->StructName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase)
		|| Property->Value.Num() != 12) return false;
	int32 Values[3];
	FMemory::Memcpy(Values, Property->Value.GetData(), sizeof(Values));
	Out = FRotator(Values[0], Values[1], Values[2]);
	return true;
}

bool ConfigureActorRotator(AWoTActorRotator& Rotator,
	const FWoTRuntimeClassDefaults& D, const FWoTRuntimeWorldActor& A)
{
	return ReadFloat(D,A,TEXT("MinRadius"),Rotator.MinRadius)
		&& ReadFloat(D,A,TEXT("MaxRadius"),Rotator.MaxRadius)
		&& ReadFloat(D,A,TEXT("RadiusShiftRate"),Rotator.RadiusShiftRate)
		&& ReadBool(D,A,TEXT("bBounceRadius"),Rotator.bBounceRadius)
		&& ReadFloat(D,A,TEXT("MinRotationRate"),Rotator.MinRotationRate)
		&& ReadFloat(D,A,TEXT("MaxRotationRate"),Rotator.MaxRotationRate)
		&& ReadFloat(D,A,TEXT("RotationShiftRate"),Rotator.RotationShiftRate)
		&& ReadBool(D,A,TEXT("bBounceRotationRate"),Rotator.bBounceRotationRate)
		&& ReadFloat(D,A,TEXT("MinHeight"),Rotator.MinHeight)
		&& ReadFloat(D,A,TEXT("MaxHeight"),Rotator.MaxHeight)
		&& ReadFloat(D,A,TEXT("HeightShiftRate"),Rotator.HeightShiftRate)
		&& ReadBool(D,A,TEXT("bBounceHeight"),Rotator.bBounceHeight)
		&& ReadBool(D,A,TEXT("bUpdateActorRotation"),Rotator.bUpdateActorRotation)
		&& ReadRotatorOffsetUnits(D,A,Rotator.ActorRotationOffset)
		&& ReadBool(D,A,TEXT("bFaceIn"),Rotator.bFaceIn)
		&& ReadBool(D,A,TEXT("bFaceOut"),Rotator.bFaceOut);
}

void ProjectActorRotators(UWorld& World,
	const FWoTWorldProjectionSource& Source, const FWoTRuntimeClassDefaults& D,
	const TMap<FString, TArray<AActor*>>& ActorsByRecord,
	FWoTWorldProjectionResult& Out)
{
	for (int32 Index = 0; Index < Source.Records.Num(); ++Index)
	{
		const FWoTRuntimeWorldActor& A = Source.Records[Index];
		if (!Leaf(A.ClassIdentity).Equals(
			TEXT("ActorRotator"), ESearchCase::IgnoreCase)) continue;
		FName Event, OwnTag;
		bool bLongDistance = false;
		FTransform Transform;
		if (!ActorTransform(D,A,Transform)
			|| !ReadName(D,A,TEXT("Event"),Event)
			|| !ReadName(D,A,TEXT("Tag"),OwnTag)
			|| !ReadBool(D,A,TEXT("bLongDistance"),bLongDistance))
		{
			SkipActor(Out,ActorRotatorRule,A.Identity,
				TEXT("an actor rotator link property is malformed"));
			continue;
		}
		struct FDriven { AActor* Actor; FVector Origin; };
		TArray<FDriven> Driven;
		if (!Event.IsNone())
		{
			for (const FWoTRuntimeWorldActor& Candidate : Source.Records)
			{
				if (Candidate.Identity.Equals(A.Identity, ESearchCase::IgnoreCase))
					continue;
				FName CandidateTag;
				if (!ReadName(D,Candidate,TEXT("Tag"),CandidateTag)
					|| CandidateTag != Event
					|| (!OwnTag.IsNone() && CandidateTag == OwnTag)) continue;
				const TArray<AActor*>* Projected =
					ActorsByRecord.Find(Candidate.Identity.ToLower());
				if (!Projected) continue;
				FVector Origin = FVector::ZeroVector;
				if (!ReadVector(D,Candidate,TEXT("Location"),Origin)) continue;
				for (AActor* Actor : *Projected)
					if (Actor && !Driven.ContainsByPredicate(
						[Actor](const FDriven& Item){ return Item.Actor == Actor; }))
						Driven.Add({Actor,Origin});
			}
		}
		if (Driven.IsEmpty())
		{
			SkipActor(Out,ActorRotatorRule,A.Identity,
				TEXT("no projected actor matches the rotator Event->Tag link"));
			continue;
		}
		const int32 GroupCount = bLongDistance ? 1 : Driven.Num();
		for (int32 Group = 0; Group < GroupCount; ++Group)
		{
			AWoTActorRotator* Rotator = SpawnDeferred<AWoTActorRotator>(
				World, Index, ActorRotatorRule, Transform,
				GroupCount == 1 ? INDEX_NONE : Group);
			if (!Rotator || !ConfigureActorRotator(*Rotator,D,A))
			{
				if (Rotator) Rotator->Destroy();
				SkipActor(Out,ActorRotatorRule,A.Identity,
					TEXT("an actor rotator property is malformed"));
				continue;
			}
			// ActorRotator.Copy omits these template flags. Only the original
			// rotator for the first driven actor retains the authored values.
			if (!bLongDistance && Group > 0)
			{
				Rotator->bFaceIn = false;
				Rotator->bFaceOut = false;
				Rotator->bBounceRadius = false;
				Rotator->bBounceRotationRate = false;
				Rotator->bBounceHeight = false;
			}
			Rotator->SetReplicates(false);
			const int32 First = bLongDistance ? 0 : Group;
			const int32 Last = bLongDistance ? Driven.Num() : Group + 1;
			for (int32 DrivenIndex = First; DrivenIndex < Last; ++DrivenIndex)
			{
				AActor* DrivenActor = Driven[DrivenIndex].Actor;
				Rotator->DrivenActors.Add(TSoftObjectPtr<AActor>(DrivenActor));
				if (bLongDistance) Rotator->DrivenOrigins.Add(Driven[DrivenIndex].Origin);
				if (USceneComponent* Root = DrivenActor->GetRootComponent())
					Root->SetMobility(EComponentMobility::Movable);
			}
			Out.DeferredActors.Add(Rotator);
			Count(Out,ActorRotatorRule);
		}
	}
}

bool ProjectZone(UWorld& World, const FWoTRuntimeWorldZone& Z, int32 ZoneIndex,
	const FWoTWorldProjectionSource& Source, const FWoTRuntimeClassDefaults& D,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	const int32 RecordIndex = FindRecord(Source.Records, Z.ActorIdentity);
	if (RecordIndex == INDEX_NONE) return true;
	const FWoTRuntimeWorldActor& A = Source.Records[RecordIndex];
	const bool bBaseZone = IsExact(A.ClassIdentity, TEXT("Engine.ZoneInfo"));
	const bool bWotZone = IsExact(A.ClassIdentity, TEXT("WOT.WOTZoneInfo"));
	const bool bWaterZone = IsExact(A.ClassIdentity, TEXT("WOT.WaterZone"));
	if (!bBaseZone && !bWotZone && !bWaterZone) return true;
	FVector Min, Max;
	if (!NodeBounds(Source, ZoneIndex, Min, Max)) return true;
	bool Water = false, Pit = false, Pain = false, Heal = false, Fog = false, AllowEditing = true;
	bool FOV = false, Leech = false, Destructive = false;
	FVector Velocity = FVector::ZeroVector, Gravity(0, 0, -950), ViewFog = FVector::ZeroVector,
		ViewFlash = FVector::ZeroVector;
	float Buoyancy = 1, GroundFriction = 8, FluidFriction = 1.2f, ViewDistance = 32;
	int32 Damage = 0, HealRate = 0, MaxCarcasses = 3, Team = 255, MinDistance = 0;
	FRotator ViewAngle(-45, 0, 0);
	float PainInterval = 1.0f;
	FName DamageType;
	FName EntrySound;
	FName ExitSound;
	FName HealSound;
	FName ZonePlayerEvent;
	float ZoneFOV = 90.0f;
	TArray<FName> LeechClassNames;
	TArray<float> TimerDurations;
	FName TimerIcon;
	bool PersistentTimer = false, RemoveTimerOnExit = false;
	bool TimerOnRight = false, OnlyTimeOnce = false;
	if (!ReadBool(D,A,TEXT("bWaterZone"),Water) || !ReadBool(D,A,TEXT("bPitZone"),Pit)
		|| !ReadBool(D,A,TEXT("bPainZone"),Pain)
		|| !ReadBool(D,A,TEXT("bDestructive"),Destructive)
		|| !ReadBool(D,A,TEXT("bHealZone"),Heal) || !ReadBool(D,A,TEXT("bFogZone"),Fog)
		|| !ReadBool(D,A,TEXT("bFOVZone"),FOV)
		|| !ReadFloat(D,A,TEXT("ZoneFOV"),ZoneFOV)
		|| !ReadBool(D,A,TEXT("bLeechZone"),Leech)
		|| !ReadObjectLeafSlots(D,A,TEXT("LeechClasses"),16,LeechClassNames)
		|| !ReadFloatSlots(D,A,TEXT("TimerLeechDuration"),3,TimerDurations,true)
		|| !ReadObjectSubPath(D,A,TEXT("TimerLeechIcon"),TimerIcon,
			EWoTNeutralResourceKind::Texture)
		|| !ReadBool(D,A,TEXT("bPersistentTimer"),PersistentTimer)
		|| !ReadBool(D,A,TEXT("bRemoveTimerOnExit"),RemoveTimerOnExit)
		|| !ReadBool(D,A,TEXT("bTimerOnRight"),TimerOnRight)
		|| !ReadBool(D,A,TEXT("bOnlyTimeOnce"),OnlyTimeOnce)
		|| !ReadBool(D,A,TEXT("bAllowEditing"),AllowEditing)
		|| !ReadVector(D,A,TEXT("ZoneVelocity"),Velocity) || !ReadVector(D,A,TEXT("ZoneGravity"),Gravity)
		|| !ReadVector(D,A,TEXT("ViewFog"),ViewFog)
		|| !ReadFloat(D,A,TEXT("ZoneGroundFriction"),GroundFriction)
		|| !ReadFloat(D,A,TEXT("ZoneFluidFriction"),FluidFriction)
		|| !ReadFloat(D,A,TEXT("Buoyancy"),Buoyancy)
		|| !ReadInt(D,A,TEXT("DamagePerSec"),Damage) || !ReadInt(D,A,TEXT("HealPerSec"),HealRate)
		|| !ReadInt(D,A,TEXT("MaxCarcasses"),MaxCarcasses)
		|| !ReadInt(D,A,TEXT("Team"),Team) || !ReadInt(D,A,TEXT("MinEditDistance"),MinDistance)
		|| !ReadFloat(D,A,TEXT("EditViewDistance"),ViewDistance)
		|| !ReadRotator(D,A,TEXT("EditViewAngle"),ViewAngle)
		|| !ReadFloat(D,A,TEXT("PainInterval"),PainInterval)
		|| !ReadName(D,A,TEXT("DamageType"),DamageType)
		|| !ReadObjectSubPath(D,A,TEXT("EntrySound"),EntrySound)
		|| !ReadObjectSubPath(D,A,TEXT("ExitSound"),ExitSound)
		|| !ReadObjectSubPath(D,A,TEXT("HealSound"),HealSound)
		|| !ReadName(D,A,TEXT("ZonePlayerEvent"),ZonePlayerEvent))
	{ Error = TEXT("a zone projection has malformed properties"); return false; }
	// ViewFlash is absent from the immutable world-property pin. The -0.078 value
	// belongs to WaterZone.uc's CLASS default, not to Actor.bWaterZone: shipped
	// fire/lava WOTZoneInfo instances set that flag but retain ViewFlash=(0,0,0).
	if (bWaterZone) ViewFlash = FVector(-0.078f);
	bool TrigVelocity=false, TrigInitiallyOn=false;
	if (!ReadBool(D,A,TEXT("bTrigZoneVelo2"),TrigVelocity)
		|| !ReadBool(D,A,TEXT("bTrigZoneVelo2InitiallyOn"),TrigInitiallyOn))
	{ Error = TEXT("a zone trigger velocity flag is malformed"); return false; }
	if (TrigVelocity)
	{
		FVector SelectedVelocity = FVector::ZeroVector;
		if (!ReadVector(D,A, TrigInitiallyOn
			? TEXT("MaxZoneVelocity2") : TEXT("MinZoneVelocity2"), SelectedVelocity))
		{ Error = TEXT("a zone trigger velocity is malformed"); return false; }
		Velocity = SelectedVelocity;
	}
	const FWoTRuntimeWorldProperty* AuthoredGravity =
		WoTRuntimeWorldFindProperty(A, TEXT("ZoneGravity"));
	if (AuthoredGravity && AuthoredGravity->Type != EWoTRuntimeWorldPropertyType::Struct)
	{ Error = TEXT("an authored zone gravity is malformed"); return false; }
	const bool bTimer = TimerDurations.ContainsByPredicate(
		[](float Value){ return Value > 0.0f; });
	const bool Effect = Water || Pit || Pain || Heal || Fog || FOV || Leech
		|| !ZonePlayerEvent.IsNone()
		|| Destructive || bTimer || !Velocity.IsNearlyZero()
		|| !Gravity.Equals(FVector(0,0,-950)) || !FMath::IsNearlyEqual(GroundFriction,8)
		|| !FMath::IsNearlyEqual(FluidFriction,1.2f) || !ViewFog.IsNearlyZero()
		|| !ViewFlash.IsNearlyZero();
	const bool Citadel = !AllowEditing || Team != 255 || MinDistance != 0
		|| !FMath::IsNearlyEqual(ViewDistance,32) || !ViewAngle.Equals(FRotator(-45,0,0));
	if (bBaseZone && !Effect && !Citadel) return true;
	AWoTZoneVolume* Volume = SpawnDeferred<AWoTZoneVolume>(World, RecordIndex, ZoneRule,
		FTransform((Min + Max) * .5));
	if (!Volume) { Error = TEXT("a zone projection could not be allocated"); return false; }
	Volume->SetReplicates(false); Volume->Collision->SetBoxExtent(((Max-Min)*.5).ComponentMax(FVector(1)), false);
	Volume->bWaterZone=Water; Volume->bPitZone=Pit; Volume->bPainZone=Pain; Volume->bHealZone=Heal; Volume->bFogZone=Fog;
	Volume->ProjectedZoneIndex=ZoneIndex;
	Volume->bFOVZone=FOV; Volume->ZoneFOV=ZoneFOV; Volume->bLeechZone=Leech;
	Volume->ZonePlayerEvent=ZonePlayerEvent;
	for (const FName IdentityName : LeechClassNames)
	{
		if (IdentityName.IsNone()) continue;
		const FString Identity = IdentityName.ToString();
		if (Identity.Equals(TEXT("WOT.SmoothRandFOVLeech"),ESearchCase::IgnoreCase))
			Volume->LeechClasses.Add(UWoTSmoothRandFOVLeech::StaticClass());
		else if (Identity.Equals(TEXT("WOT.WaterFOVLeech"),ESearchCase::IgnoreCase))
			Volume->LeechClasses.Add(UWoTWaterFOVLeech::StaticClass());
		else if (Identity.Equals(TEXT("WOT.RandFOVLeech"),ESearchCase::IgnoreCase))
			Volume->LeechClasses.Add(UWoTRandFOVLeech::StaticClass());
		else if (Identity.Equals(TEXT("WOT.TimerLeech"),ESearchCase::IgnoreCase))
			Volume->LeechClasses.Add(UWoTTimerLeech::StaticClass());
	}
	Volume->TimerLeechDuration=TimerDurations;
	if (!TimerIcon.IsNone())
	{
		const FString Sub = TimerIcon.ToString();
		const FString LeafName = FPaths::GetBaseFilename(Sub);
		Volume->TimerLeechIcon = FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/%s.%s"), *Sub, *LeafName));
	}
	Volume->bPersistentTimer=PersistentTimer;
	Volume->bRemoveTimerOnExit=RemoveTimerOnExit;
	Volume->bTimerOnRight=TimerOnRight;
	Volume->bOnlyTimeOnce=OnlyTimeOnce;
	Volume->ViewFog=ViewFog;
	Volume->ViewFlash=ViewFlash;
	Volume->Buoyancy=Buoyancy; Volume->ZoneVelocity=Velocity; Volume->ZoneGravity=Gravity;
	Volume->bOverrideGravity=AuthoredGravity && !Gravity.IsNearlyZero(); Volume->DamagePerSec=Damage; Volume->bDestructive=Destructive; Volume->HealPerSec=HealRate;
	Volume->MaxCarcasses=MaxCarcasses; Volume->ZoneGroundFriction=GroundFriction;
	Volume->ZoneFluidFriction=FluidFriction; Volume->EditTeam=FMath::Clamp(Team,0,255);
	Volume->MinEditDistance=FMath::Clamp(MinDistance,0,255); Volume->EditViewDistance=ViewDistance;
	Volume->EditViewAngle=ViewAngle; Volume->bAllowEditing=AllowEditing;
	Volume->DamageType=DamageType;
	Volume->PainInterval=PainInterval;
	Volume->EntrySoundSubPath=EntrySound;
	Volume->ExitSoundSubPath=ExitSound;
	Volume->HealSoundPath=HealSound;
	Out.DeferredActors.Add(Volume); Count(Out, ZoneRule);
	if (!EntrySound.IsNone() || !ExitSound.IsNone()) Count(Out, WaterSoundZoneRule);
	return true;
}

bool ProjectLight(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error, const TSet<FName>& LightningTags)
{
	uint8 Type=1, Brightness=64, Hue=0, Saturation=255, Radius=64;
	bool Special=false;
	FName Tag;
	FTransform T;
	if (!ActorTransform(D,A,T) || !ReadByte(D,A,TEXT("LightType"),Type)
		|| !ReadByte(D,A,TEXT("LightBrightness"),Brightness)
		|| !ReadByte(D,A,TEXT("LightHue"),Hue)
		|| !ReadByte(D,A,TEXT("LightSaturation"),Saturation)
		|| !ReadByte(D,A,TEXT("LightRadius"),Radius)
		|| !ReadBool(D,A,TEXT("bSpecialLit"),Special)
		|| !ReadName(D,A,TEXT("Tag"),Tag))
	{ Error=TEXT("a light projection has malformed properties"); return false; }
	if (Type == 0) return true;
	if (Brightness == 0 || Special) return true;
	const bool bLightningTarget = LightningTags.Contains(Tag);
	const float Mean = UWoTDynamicLightSubsystem::RetailWaveMean(Type);
	APointLight* Light = SpawnDeferred<APointLight>(World,Index,LightRule,T);
	if (!Light) { Error=TEXT("a light projection could not be allocated"); return false; }
	Light->SetReplicates(false);
	UPointLightComponent* C = Light->PointLightComponent;
	C->SetMobility(EComponentMobility::Movable);
	C->SetCastShadows(false); C->SetIndirectLightingIntensity(0);
	C->SetUseInverseSquaredFalloff(false);
	C->SetLightFColor(ImporterPointLightColor(Hue, Saturation));
	C->SetIntensity(FMath::Pow(Brightness / 255.f * Mean, 2.2f) * 5.f);
	C->SetAttenuationRadius(FMath::Max(1.f,Radius*25.f));
	C->SetLightFalloffExponent(5.3f);
	C->LightingChannels.bChannel0=false; C->LightingChannels.bChannel1=true; C->LightingChannels.bChannel2=false;
	if (bLightningTarget)
	{
		Light->Tags.AddUnique(Tag);
		C->SetVisibility(false);
		UWoTDynamicLightSubsystem::RegisterOrDefer(C, Brightness, Hue, Saturation);
	}
	Out.DeferredActors.Add(Light);
	if (!bLightningTarget) Count(Out,LightRule);
	return true;
}

bool ProjectSprayer(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FWoTRuntimeParticleSprayerConfiguration Config;
	FName Tag;
	if(!WoTRuntimeReadParticleSprayerConfiguration(
		[&](FStringView Name,uint32 Slot){return D.FindProperty(A,Name,Slot);},Config)
		|| !ReadName(D,A,TEXT("Tag"),Tag))
	{
		++Out.SkippedSprayerCount;
		SkipActor(Out, ParticleSprayerRule, A.Identity,
			TEXT("particle configuration is malformed or unsupported"));
		return true;
	}
	const bool bTriggered=Config.InitialState==TEXT("TriggerToggle")
		|| Config.InitialState==TEXT("TriggerControl")
		|| Config.InitialState==TEXT("TriggerTimed");
	if (!Config.bOn && !bTriggered)
	{
		++Out.SkippedSprayerCount;
		SkipActor(Out, ParticleSprayerRule, A.Identity,
			TEXT("particle sprayer is authored off"));
		return true;
	}
	// Map sprayers are presentation-only on a dedicated server. Preserve the
	// actor/configuration for event topology, but stop before resolving any
	// animation texture or material and before BeginPlay can allocate MIDs.
	if (World.GetNetMode() == NM_DedicatedServer)
	{
		AWoTParticleSprayer* S = SpawnDeferred<AWoTParticleSprayer>(
			World, Index, ParticleSprayerRule, Config.Transform);
		if (!S)
		{
			Error = TEXT("a particle sprayer projection could not be allocated");
			return false;
		}
		S->SetReplicates(false);
		WoTRuntimeApplyParticleSprayerConfiguration(Config, *S);
		S->TagName = Tag;
		S->PrimeCount = FMath::Max(S->PrimeCount, 90.0f);
		S->bFreezeOutsideVisibility = true;
		Out.DeferredActors.Add(S);
		Count(Out, ParticleSprayerRule);
		return true;
	}
	TArray<TObjectPtr<UMaterialInterface>> Adds, Mods;
	Adds.SetNum(Config.Templates.Num()); Mods.SetNum(Config.Templates.Num());
	const bool bStyleModulated = Config.Style == 4;
	TArray<TArray<FString>> AnimationFrames;
	TArray<TArray<TObjectPtr<UTexture2D>>> AnimationTextures;
	TArray<float> AnimationMinRates;
	TArray<float> AnimationMaxRates;
	AnimationFrames.SetNum(Config.Templates.Num());
	AnimationTextures.SetNum(Config.Templates.Num());
	AnimationMinRates.Init(0.0f, Config.Templates.Num());
	AnimationMaxRates.Init(0.0f, Config.Templates.Num());
	for (int32 Slot=0; Slot<Config.Templates.Num(); ++Slot)
	{
		if (Source.FindTexture)
		{
			FString Identity = Config.TextureIdentities[Slot];
			TSet<FString> Seen;
			for (int32 Frame = 0; Frame < 1024; ++Frame)
			{
				if (Seen.Contains(Identity)) break;
				Seen.Add(Identity);
				FWoTRetailTextureFacts Facts;
				if (!Source.FindTexture(Identity, Facts))
				{
					Error = FString::Printf(TEXT("particle animation texture is unavailable (%s)"),
						*Identity);
					return false;
				}
				if (AnimationFrames[Slot].IsEmpty())
				{
					AnimationMinRates[Slot] = Facts.MinFrameRate;
					AnimationMaxRates[Slot] = Facts.MaxFrameRate;
				}
				AnimationFrames[Slot].Add(Identity);
				if (Facts.AnimNext.IsEmpty()) break;
				Identity = Facts.AnimNext;
			}
		}
		// Exact retail identity: a colliding export (ParticleSystems
		// .Blood vs .ModulatedBlood Blood01) only exists grouped, so both the
		// texture and the derived Add/Mod instances follow the exact route.
		FWoTRetailObjectPath RetailPath;
		if (!WoTParseRetailObjectPath(Config.TextureIdentities[Slot], RetailPath))
		{
			++Out.SkippedSprayerCount;
			SkipActor(Out, ParticleSprayerRule, A.Identity,
				TEXT("particle texture identity is malformed"));
			return true;
		}
		const FString TextureSubPath=WoTRetailTextureSubPath(RetailPath);
		const bool bGrouped=TextureSubPath!=RetailPath.Package+TEXT("/")+RetailPath.Name;
		const FString& Name=RetailPath.Name;
		const FString Slug=WoTRetailDerivedMaterialSuffix(RetailPath,bGrouped);
		const FWoTRuntimeWorldProperty* ParticleProperty =
			D.FindProperty(A, TEXT("Particles"), Slot);
		const bool bProcedural = ParticleProperty
			&& ParticleProperty->ObjectClassIdentity.Contains(
				TEXT("FireTexture"), ESearchCase::IgnoreCase);
		const TCHAR* AddPrefix = bProcedural ? TEXT("MI_PSAddFB_") : TEXT("MI_PSAdd_");
		const TCHAR* ModPrefix = bProcedural ? TEXT("MI_PSModFB_") : TEXT("MI_PSMod_");
		FWoTRuntimeTextureOptions TextureOptions; TextureOptions.bLoadMips=true;
		TextureOptions.bSRGB=false; TextureOptions.Filter=TF_Bilinear;
		for (const FString& FrameIdentity : AnimationFrames[Slot])
		{
			FWoTRetailObjectPath FramePath;
			if (!WoTParseRetailObjectPath(FrameIdentity, FramePath))
			{
				Error = TEXT("a particle animation frame identity is malformed");
				return false;
			}
			const FString FrameSubPath = WoTRetailTextureSubPath(FramePath);
#if WITH_AUTOMATION_TESTS
			WoTCountTextureAnimationResolutionForTesting();
#endif
			UTexture2D* FrameTexture = UWoTRuntimeAssets::ResolveTexturePath(
				FSoftObjectPath(FString::Printf(TEXT("/Game/WoT/Textures/%s.%s"),
					*FrameSubPath, *FramePath.Name)), TextureOptions);
			if (FrameTexture == nullptr)
			{
				const bool bFrameGrouped = FrameSubPath
					!= FramePath.Package + TEXT("/") + FramePath.Name;
				const FString FrameSlug = WoTRetailDerivedMaterialSuffix(
					FramePath, bFrameGrouped);
				UMaterialInterface* FrameMaterial =
					UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(
						FString::Printf(
							TEXT("/Game/WoT/Materials/Instances/%s%s.%s%s"),
							bStyleModulated ? ModPrefix : AddPrefix, *FrameSlug,
							bStyleModulated ? ModPrefix : AddPrefix, *FrameSlug)));
				if (FrameMaterial)
				{
					UTexture* MaterialTexture = nullptr;
					FrameMaterial->GetTextureParameterValue(
						FHashedMaterialParameterInfo(TEXT("Tex")), MaterialTexture);
					FrameTexture = Cast<UTexture2D>(MaterialTexture);
				}
			}
			if (FrameTexture == nullptr)
			{
				Error = FString::Printf(
					TEXT("particle animation frame texture is unavailable (%s)"),
					*FrameIdentity);
				return false;
			}
			AnimationTextures[Slot].Add(FrameTexture);
		}
		const FString AddPath=FString::Printf(
			TEXT("/Game/WoT/Materials/Instances/%s%s.%s%s"),
			AddPrefix,*Slug,AddPrefix,*Slug);
		const FString ModPath=FString::Printf(
			TEXT("/Game/WoT/Materials/Instances/%s%s.%s%s"),
			ModPrefix,*Slug,ModPrefix,*Slug);
		const FString TexturePath=FString::Printf(
			TEXT("/Game/WoT/Textures/%s.%s"),*TextureSubPath,*Name);
		if ((!bStyleModulated && !FitsProjectedName(AddPath))
			|| !FitsProjectedName(ModPath)
			|| !FitsProjectedName(TexturePath))
		{
			++Out.SkippedSprayerCount;
			SkipActor(Out, ParticleSprayerRule, A.Identity,
				TEXT("particle texture identity is malformed"));
			return true;
		}
		UTexture2D* Texture = AnimationTextures[Slot].IsEmpty() ? nullptr
			: AnimationTextures[Slot][0].Get();
		if (Texture == nullptr)
		{
#if WITH_AUTOMATION_TESTS
			WoTCountTextureAnimationResolutionForTesting();
#endif
			Texture=UWoTRuntimeAssets::ResolveTexturePath(
				FSoftObjectPath(TexturePath),TextureOptions);
		}
		if (bStyleModulated)
		{
			Adds[Slot] = nullptr;
			Mods[Slot] = UWoTRuntimeAssets::CreateModulatedSpriteMaterial(
				&World, FSoftObjectPath(TexturePath));
			if (!Mods[Slot])
			{
				++Out.SkippedSprayerCount;
				SkipActor(Out, ParticleSprayerRule, A.Identity,
					FString::Printf(
						TEXT("particle modulate material is unavailable (%s)"),
						*Config.TextureIdentities[Slot]));
				return true;
			}
		}
		else
		{
			Adds[Slot]=UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(AddPath));
			Mods[Slot]=UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(ModPath));
			// Development content intentionally omits many mechanically-derived
			// MI_PSAdd_/MI_PSMod_ assets. Guarded builds derive them from the cache;
			// do the same in editor projection when the authenticated texture exists.
			if ((!Adds[Slot] || !Mods[Slot]) && Texture && !bProcedural)
			{
				UMaterialInterface* AddMaster=Cast<UMaterialInterface>(FSoftObjectPath(
					TEXT("/Game/WoT/Materials/M_WoT_ParticleAdd.M_WoT_ParticleAdd")).TryLoad());
				UMaterialInterface* ModMaster=Cast<UMaterialInterface>(FSoftObjectPath(
					TEXT("/Game/WoT/Materials/M_WoT_ParticleMod.M_WoT_ParticleMod")).TryLoad());
				UMaterialInstanceDynamic* AddMID=AddMaster
					? UMaterialInstanceDynamic::Create(AddMaster,&World) : nullptr;
				UMaterialInstanceDynamic* ModMID=ModMaster
					? UMaterialInstanceDynamic::Create(ModMaster,&World) : nullptr;
#if WITH_AUTOMATION_TESTS
				if (AnimationFrames[Slot].Num() > 1)
				{
					if (AddMID) WoTCountTextureAnimationMIDForTesting();
					if (ModMID) WoTCountTextureAnimationMIDForTesting();
				}
#endif
				if(AddMID)AddMID->SetTextureParameterValue(TEXT("Tex"),Texture);
				if(ModMID)ModMID->SetTextureParameterValue(TEXT("Tex"),Texture);
				Adds[Slot]=AddMID;Mods[Slot]=ModMID;
			}
			if (!Adds[Slot] || !Mods[Slot] || (!Texture && !bProcedural))
			{
				++Out.SkippedSprayerCount;
				const FString Missing = FString::Printf(
					TEXT("particle texture or material is unavailable (%s)"),
					*Config.TextureIdentities[Slot]);
				SkipActor(Out, ParticleSprayerRule, A.Identity, Missing);
				return true;
			}
		}
		if(Texture)
		{
			Config.Templates[Slot].TexWidth=Texture->GetSizeX();
			Config.Templates[Slot].TexHeight=Texture->GetSizeY();
		}
	}
	AWoTParticleSprayer* S=SpawnDeferred<AWoTParticleSprayer>(World,Index,ParticleSprayerRule,Config.Transform);
	if (!S) { Error=TEXT("a particle sprayer projection could not be allocated"); return false; }
	S->SetReplicates(false); WoTRuntimeApplyParticleSprayerConfiguration(Config,*S);
	S->TagName=Tag;
	S->AddMaterials=MoveTemp(Adds); S->ModMaterials=MoveTemp(Mods);
	for (int32 Slot = 0; Slot < AnimationFrames.Num(); ++Slot)
	{
		if (AnimationFrames[Slot].Num() > 1)
		{
			S->ConfigureTemplateAnimation(Slot, Config.TextureIdentities[Slot],
				MoveTemp(AnimationFrames[Slot]), MoveTemp(AnimationTextures[Slot]),
				AnimationMinRates[Slot],
				AnimationMaxRates[Slot]);
		}
	}
	S->PrimeCount=FMath::Max(S->PrimeCount,90.f);
	S->bFreezeOutsideVisibility=true;
	Out.DeferredActors.Add(S); Count(Out,ParticleSprayerRule); return true;
}

bool ProjectLightningFX(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, bool bRandom,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	FTransform T; FName Tag, Event, LightTag;
	if (!ActorTransform(D,A,T) || !ReadName(D,A,TEXT("Tag"),Tag)
		|| !ReadName(D,A,TEXT("Event"),Event)
		|| !ReadName(D,A,TEXT("LightTag"),LightTag))
	{ Error=TEXT("a lightning record is malformed"); return false; }
	AWoTLightning* L = bRandom
		? static_cast<AWoTLightning*>(SpawnDeferred<AWoTRandomLightning>(World,Index,RandomLightningRule,T))
		: SpawnDeferred<AWoTLightning>(World,Index,LightningRule,T);
	if (!L) { Error=TEXT("a lightning projection could not be allocated"); return false; }
	L->TagName=Tag; L->EventName=Event; L->LightTag=LightTag;
	if (bRandom)
	{
		AWoTRandomLightning* R=CastChecked<AWoTRandomLightning>(L);
		if (!ReadFloat(D,A,TEXT("MinThunderDelay"),R->MinThunderDelay)
			|| !ReadFloat(D,A,TEXT("MaxThunderDelay"),R->MaxThunderDelay)
			|| !ReadFloat(D,A,TEXT("MinLightningTime"),R->MinLightningTime)
			|| !ReadFloat(D,A,TEXT("MaxLightningTime"),R->MaxLightningTime)
			|| !ReadBool(D,A,TEXT("bSpawnsLightning"),R->bSpawnsLightning))
		{ Error=TEXT("a RandomLightning record is malformed"); return false; }
	}
	else
	{
		FString Thunder=TEXT("Environment.Thunder10");
		if (!ReadFloat(D,A,TEXT("ThunderDelay"),L->ThunderDelay)
			|| !ReadFloat(D,A,TEXT("HalfLightPct"),L->HalfLightPct)
			|| !ReadFloat(D,A,TEXT("BrightnessPerSecond"),L->BrightnessPerSecond)
			|| !ReadFloat(D,A,TEXT("LightningLength"),L->LightningLength)
			|| !ReadFloat(D,A,TEXT("ThunderRadius"),L->ThunderRadius)
			|| !ReadString(D,A,TEXT("ThunderSound"),Thunder))
		{ Error=TEXT("a Lightning record is malformed"); return false; }
		Thunder.ReplaceInline(TEXT("."),TEXT("/")); L->ThunderSound=FName(*Thunder);
	}
	Out.DeferredActors.Add(L); Count(Out,bRandom?RandomLightningRule:LightningRule); return true;
}

bool ProjectDistanceLightning(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform T;
	if (!ActorTransform(D,A,T))
	{
		Error=TEXT("a DistanceLightning record is malformed"); return false;
	}
	AWoTDistanceLightning* L=SpawnDeferred<AWoTDistanceLightning>(
		World,Index,DistanceLightningRule,T);
	if(!L){Error=TEXT("a DistanceLightning projection could not be allocated");return false;}
	Out.DeferredActors.Add(L); Count(Out,DistanceLightningRule); return true;
}

bool ProjectWind(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform T; AWoTWind* W=nullptr;
	FName Tag,Event,State; bool InitiallyOn=true;
	if (!ActorTransform(D,A,T)||!ReadName(D,A,TEXT("Tag"),Tag)
		||!ReadName(D,A,TEXT("Event"),Event)||!ReadName(D,A,TEXT("InitialState"),State)
		||!ReadBool(D,A,TEXT("bInitiallyOn"),InitiallyOn))
	{ Error=TEXT("a Wind record is malformed"); return false; }
	W=SpawnDeferred<AWoTWind>(World,Index,WindRule,T);
	if(!W){Error=TEXT("a Wind projection could not be allocated");return false;}
	W->TagName=Tag;W->EventName=Event;W->InitialState=State;W->bInitiallyOn=InitiallyOn;
	if(!ReadFloat(D,A,TEXT("MinImpulseTime"),W->MinImpulseTime)
		||!ReadFloat(D,A,TEXT("MaxImpulseTime"),W->MaxImpulseTime)
		||!ReadFloat(D,A,TEXT("MinWindspeed"),W->MinWindspeed)
		||!ReadFloat(D,A,TEXT("MaxWindspeed"),W->MaxWindspeed)
		||!ReadFloat(D,A,TEXT("TimerDuration"),W->TimerDuration))
	{Error=TEXT("a Wind record has malformed timing");return false;}
	Out.DeferredActors.Add(W);Count(Out,WindRule);return true;
}

bool ProjectSpew(UWorld& World, const FWoTRuntimeClassDefaults& D,
	const FWoTRuntimeWorldActor& A, int32 Index, FWoTWorldProjectionResult& Out,
	FString& Error)
{
	FTransform T;FName Tag,State;bool Enabled=false,Tumble=true,Inherit=false;
	bool Collision=false,Silent=false; float VisibilityHeight=1024,VisibilityRadius=1024;
	float Delay=1;int32 CountValue=1;FRotator Spread=FRotator::ZeroRotator;
	if(!ActorTransform(D,A,T)||!ReadName(D,A,TEXT("Tag"),Tag)
		||!ReadName(D,A,TEXT("InitialState"),State)||!ReadBool(D,A,TEXT("bEnabled"),Enabled)
		||!ReadBool(D,A,TEXT("bTumble"),Tumble)||!ReadBool(D,A,TEXT("bInheritVelocity"),Inherit)
		||!ReadBool(D,A,TEXT("bCollision"),Collision)||!ReadBool(D,A,TEXT("bSilent"),Silent)
		||!ReadFloat(D,A,TEXT("SpewVisibilityHeight"),VisibilityHeight)
		||!ReadFloat(D,A,TEXT("SpewVisibilityRadius"),VisibilityRadius)
		||!ReadFloat(D,A,TEXT("TimeBetweenSpewsSecs"),Delay)||!ReadInt(D,A,TEXT("SpewCount"),CountValue)
		||!ReadRotator(D,A,TEXT("SpewRotationSpread"),Spread))
	{Error=TEXT("a SpewTrigger record is malformed");return false;}
	AWoTSpewTrigger* S=SpawnDeferred<AWoTSpewTrigger>(World,Index,SpewTriggerRule,T);
	if(!S){Error=TEXT("a SpewTrigger projection could not be allocated");return false;}
	S->TagName=Tag;S->InitialState=State;S->bEnabled=Enabled;S->bTumble=Tumble;
	S->bInheritVelocity=Inherit;S->TimeBetweenSpewsSecs=Delay;S->SpewCount=CountValue;
	S->SpewRotationSpread=Spread;S->bCollision=Collision;S->bSilent=Silent;
	S->SpewVisibilityHeight=VisibilityHeight;S->SpewVisibilityRadius=VisibilityRadius;
	for(uint32 Slot=0;Slot<16;++Slot)
	{
		const FWoTRuntimeWorldProperty* P=D.FindProperty(A,TEXT("Things"),Slot);
		if(!P)continue;
		const FWoTRuntimeWorldProperty* ActorP=WoTRuntimeWorldStructMember(*P,TEXT("Actor"));
		if(!ActorP||ActorP->ObjectIdentity.IsEmpty())continue;
		FWoTSpewThing Thing;Thing.ClassIdentity=FName(*ActorP->ObjectIdentity);
		auto ReadMemberFloat=[&](const TCHAR* N,float& V)
		{
			const auto* M=WoTRuntimeWorldStructMember(*P,N);return !M||WoTRuntimeWorldPropertyFloat(*M,V);
		};
		auto ReadMemberInt=[&](const TCHAR* N,int32& V)
		{
			const auto* M=WoTRuntimeWorldStructMember(*P,N);return !M||WoTRuntimeWorldPropertyInteger(*M,V);
		};
		auto ReadMemberByte=[&](const TCHAR* N,int32& V)
		{
			const auto* M=WoTRuntimeWorldStructMember(*P,N);
			if(!M)return true;
			uint8 Byte=0;if(!WoTRuntimeWorldPropertyByte(*M,Byte))return false;V=Byte;return true;
		};
		if(!ReadMemberInt(TEXT("Count"),Thing.Count)||!ReadMemberByte(TEXT("DetailLevel"),Thing.DetailLevel)
			||!ReadMemberFloat(TEXT("MinSpeed"),Thing.MinSpeed)
			||!ReadMemberFloat(TEXT("MaxSpeed"),Thing.MaxSpeed)||!ReadMemberFloat(TEXT("DrawScale"),Thing.DrawScale))
		{Error=TEXT("a SpewTrigger Things entry is malformed");return false;}
		auto ClassFloat=[&](const TCHAR* N,float& V)->bool
		{
			const auto* M=D.FindClassProperty(*ActorP->ObjectIdentity,N);
			return M&&WoTRuntimeWorldPropertyFloat(*M,V);
		};
		auto ClassObject=[&](const TCHAR* N,FName& V)->bool
		{
			const auto* M=D.FindClassProperty(*ActorP->ObjectIdentity,N);
			if(!M||M->Type!=EWoTRuntimeWorldPropertyType::Object)return false;
			V=FName(*M->ObjectIdentity);return true;
		};
		auto OptionalClassFloat=[&](const TCHAR* N,float& V)->bool
		{
			const auto* M=D.FindClassProperty(*ActorP->ObjectIdentity,N);
			return !M||WoTRuntimeWorldPropertyFloat(*M,V);
		};
		auto OptionalClassObject=[&](const TCHAR* N,FName& V)->bool
		{
			const auto* M=D.FindClassProperty(*ActorP->ObjectIdentity,N);
			if(!M)return true;
			if(M->Type!=EWoTRuntimeWorldPropertyType::Object)return false;
			V=FName(*M->ObjectIdentity);return true;
		};
		FName MeshIdentity;
		if(!ClassObject(TEXT("Mesh"),MeshIdentity)
			||!ClassFloat(TEXT("CollisionRadius"),Thing.CollisionRadius)
			||!ClassFloat(TEXT("CollisionHeight"),Thing.CollisionHeight)
			// UE1 omits inherited zero/None values from saved class defaults.
			// Their initialized descriptor values are the decoded neutral values.
			||!OptionalClassFloat(TEXT("LifeSpan"),Thing.LifeSpan)
			||!ClassFloat(TEXT("MinBounceSoundSpeed"),Thing.MinBounceSoundSpeed)
			||!ClassFloat(TEXT("MinFirstHitSpeed"),Thing.MinFirstHitSpeed)
			||!ClassFloat(TEXT("MinBounceSpeed"),Thing.MinBounceSpeed)
			||!ClassFloat(TEXT("SpeedDampeningRatio"),Thing.SpeedDampeningRatio)
			||!ClassFloat(TEXT("RotationDampeningRatio"),Thing.RotationDampeningRatio)
			||!ClassFloat(TEXT("BaseBounceVolume"),Thing.BaseBounceVolume)
			||!ClassFloat(TEXT("BaseBouncePitch"),Thing.BaseBouncePitch)
			||!ClassFloat(TEXT("VolumePerturbPercent"),Thing.VolumePerturbPercent)
			||!ClassFloat(TEXT("PitchPerturbPercent"),Thing.PitchPerturbPercent)
			||!OptionalClassObject(TEXT("LandSound1"),Thing.LandSound))
		{Error=TEXT("a SpewTrigger Thing class descriptor is missing or malformed");return false;}
		Thing.MeshName=FName(*Leaf(MeshIdentity.ToString()));
		if(Thing.DrawScale==0.0f&&!ClassFloat(TEXT("DrawScale"),Thing.DrawScale))
		{Error=TEXT("a SpewTrigger Thing class DrawScale is missing or malformed");return false;}
		Thing.SkinIdentities.SetNum(8);
		for(uint32 SkinSlot=0;SkinSlot<8;++SkinSlot)
		{
			const auto* Skin=D.FindClassProperty(*ActorP->ObjectIdentity,TEXT("MultiSkins"),SkinSlot);
			if(Skin&&Skin->Type==EWoTRuntimeWorldPropertyType::Object)
				Thing.SkinIdentities[SkinSlot]=FName(*Skin->ObjectIdentity);
		}
		S->Things.Add(Thing);
	}
	Out.DeferredActors.Add(S);Count(Out,SpewTriggerRule);return true;
}

bool ProjectPaths(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D, FWoTWorldProjectionResult& Out, FString& Error)
{
	struct FProjectedPath
	{
		AWoTPathNode* Node = nullptr;
		TArray<int32> OutSpecIndices;
		TArray<int32> InSpecIndices;
	};
	TMap<FString, FProjectedPath> ByIdentity;
	for (int32 I=0; I<Source.Records.Num(); ++I)
	{
		const FWoTRuntimeWorldActor& A=Source.Records[I];
		if (!IsPathClass(A.ClassIdentity)) continue;
		FTransform T; bool Dark=false, Snipe=false; float Height=50; int32 Extra=0;
		if (!ActorTransform(D,A,T) || !ReadBool(D,A,TEXT("bDark"),Dark)
			|| !ReadBool(D,A,TEXT("bSnipe"),Snipe)
			|| !ReadFloat(D,A,TEXT("CollisionHeight"),Height)
			|| !ReadInt(D,A,TEXT("ExtraCost"),Extra))
		{
			SkipActor(Out, PathNodeRule, A.Identity,
				TEXT("path node has malformed properties"));
			continue;
		}
		TArray<int32> OutIndices, InIndices;
		bool bMalformedEdgeArray = false;
		for (uint32 Slot = 0; Slot < 16; ++Slot)
		{
			for (const TCHAR* Name : {TEXT("Paths"), TEXT("upstreamPaths")})
			{
				const FWoTRuntimeWorldProperty* P =
					WoTRuntimeWorldFindProperty(A, Name, Slot);
				if (!P) continue;
				int32 SpecIndex = INDEX_NONE;
				if (!WoTRuntimeWorldPropertyInteger(*P, SpecIndex)
					|| !Source.ReachSpecs.IsValidIndex(SpecIndex))
				{
					bMalformedEdgeArray = true;
					break;
				}
				(Name[0] == TEXT('P') ? OutIndices : InIndices).Add(SpecIndex);
			}
			if (bMalformedEdgeArray) break;
		}
		if (bMalformedEdgeArray)
		{
			SkipActor(Out, PathNodeRule, A.Identity,
				TEXT("path node has a malformed ReachSpec index"));
			continue;
		}
		if (!FitsProjectedName(A.ClassIdentity))
		{
			SkipActor(Out, PathNodeRule, A.Identity,
				TEXT("path node class identity is malformed"));
			continue;
		}
		AWoTPathNode* Node=SpawnDeferred<AWoTPathNode>(World,I,PathNodeRule,T);
		if (!Node)
		{
			SkipActor(Out, PathNodeRule, A.Identity,
				TEXT("path node could not be allocated"));
			continue;
		}
		Node->SetReplicates(false); Node->bDark=Dark; Node->NodeCollisionHeight=Height;
		Node->bSnipe=Snipe;
		Node->ExtraCost=Extra; Node->SourceClass=FName(*A.ClassIdentity);
		FProjectedPath& Projected = ByIdentity.Add(A.Identity.ToLower());
		Projected.Node = Node;
		Projected.OutSpecIndices = MoveTemp(OutIndices);
		Projected.InSpecIndices = MoveTemp(InIndices);
		Out.DeferredActors.Add(Node); Count(Out,PathNodeRule);
	}
	for (TPair<FString, FProjectedPath>& Pair : ByIdentity)
	{
		FProjectedPath& Projected = Pair.Value;
		for (const int32 SpecIndex : Projected.OutSpecIndices)
		{
			const FWoTRuntimeWorldReachSpec& R = Source.ReachSpecs[SpecIndex];
			FProjectedPath* End = ByIdentity.Find(R.EndIdentity.ToLower());
			if (!End) continue;
			FWoTPathEdge& Edge = Projected.Node->OutEdges.AddDefaulted_GetRef();
			Edge.Other = End->Node; Edge.Distance = R.Distance;
		}
		for (const int32 SpecIndex : Projected.InSpecIndices)
		{
			const FWoTRuntimeWorldReachSpec& R = Source.ReachSpecs[SpecIndex];
			FProjectedPath* Start = ByIdentity.Find(R.StartIdentity.ToLower());
			if (!Start) continue;
			FWoTPathEdge& Edge = Projected.Node->InEdges.AddDefaulted_GetRef();
			Edge.Other = Start->Node; Edge.Distance = R.Distance;
		}
	}
	Error.Reset();
	return true;
}

bool ProjectNavBounds(UWorld& World, const FWoTWorldProjectionSource& Source,
	FWoTWorldProjectionResult& Out, FString& Error)
{
	if (Source.Points.IsEmpty()) return true;
	FVector Min(DBL_MAX),Max(-DBL_MAX);
	for (const FVector3f& P:Source.Points) { Min=Min.ComponentMin(FVector(P)); Max=Max.ComponentMax(FVector(P)); }
	const FVector Extent=((Max-Min)*.5+FVector(100)).ComponentMax(FVector(1));
	ANavMeshBoundsVolume* V=SpawnDeferred<ANavMeshBoundsVolume>(World,0,NavBoundsRule,FTransform((Min+Max)*.5));
	if (!V) { Error=TEXT("navigation bounds projection could not be allocated"); return false; }
	V->SetReplicates(false);
	UBrushComponent* Brush=V->GetBrushComponent();
	UBodySetup* Body=Brush ? Brush->GetBodySetup() : nullptr;
	if (!Body && Brush)
	{
		Body = NewObject<UBodySetup>(Brush, TEXT("WoTRuntimeNavBody"), RF_Transient);
		Brush->BrushBodySetup = Body;
	}
	if (!Body) { Error=TEXT("navigation bounds has no brush body setup"); return false; }
	Body->AggGeom.EmptyElements(); FKBoxElem Box; Box.X=Extent.X*2; Box.Y=Extent.Y*2; Box.Z=Extent.Z*2;
	Body->AggGeom.BoxElems.Add(Box); Body->InvalidatePhysicsData(); Body->CreatePhysicsMeshes();
	Brush->RecreatePhysicsState(); Brush->UpdateBounds();
	Out.DeferredActors.Add(V); Count(Out,NavBoundsRule); return true;
}

bool FillLightTable(UWorld& World, const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D, const TSet<FName>& LightningTags,
	FWoTWorldProjectionResult& Out, float& OutSkyAmbient, FString& Error)
{
	UWoTMapLightTable* Table=NewObject<UWoTMapLightTable>(&World,NAME_None,RF_Transient);
	if (Table == nullptr)
	{
		Error = TEXT("the runtime light table could not be allocated");
		return false;
	}
	TSet<FString> RejectedRecords;
	auto Reject=[&Out,&RejectedRecords](
		const FWoTRuntimeWorldActor& Actor,FStringView Reason)
	{
		const FString Key=Actor.Identity.ToLower();
		if (RejectedRecords.Contains(Key)) return;
		RejectedRecords.Add(Key);
		SkipActor(Out,LightTableRule,Actor.Identity,Reason);
	};
	const FWoTRuntimeWorldActor* Level = nullptr;
	for (const FWoTRuntimeWorldActor& A:Source.Records)
	{
		if (IsExact(A.ClassIdentity,TEXT("Engine.LevelInfo")))
		{
			float LevelBrightness = 1.0f;
			bool bCheckWalkSurfaces = true;
			int32 IdealPlayerCount = 0;
			FString IdealPlayerCountText;
			const FWoTRuntimeWorldProperty* Ideal =
				Prop(D,A,TEXT("IdealPlayerCount"));
			if (!ReadFloat(D,A,TEXT("Brightness"),LevelBrightness)
				|| !ReadBool(D,A,TEXT("bCheckWalkSurfaces"),bCheckWalkSurfaces)
				|| (Ideal != nullptr
					&& !WoTRuntimeWorldPropertyUE1String(
						*Ideal,IdealPlayerCountText)))
			{
				Reject(A,TEXT("level light-table properties are malformed"));
				continue;
			}
			Level = &A;
			if (Ideal != nullptr)
				IdealPlayerCount=FCString::Atoi(*IdealPlayerCountText);
			Table->LevelBrightness=LevelBrightness;
			Table->bCheckWalkSurfaces=bCheckWalkSurfaces;
			Table->IdealPlayerCount=IdealPlayerCount;
			break;
		}
	}
	const int32 ZoneCount = FMath::Max(Source.Zones.Num(), 1);
	Table->ZoneMinLightCounts.Reserve(ZoneCount);
	Table->ZoneMaxLightCounts.Reserve(ZoneCount);
	for (int32 I=0; I<ZoneCount; ++I)
	{
		const FWoTRuntimeWorldActor* AmbientActor = Level;
		if (I > 0 && Source.Zones.IsValidIndex(I)
			&& !Source.Zones[I].ActorIdentity.IsEmpty())
		{
			const int32 Record = FindRecord(
				Source.Records, Source.Zones[I].ActorIdentity);
			if (Record != INDEX_NONE) AmbientActor = &Source.Records[Record];
		}
		uint8 H=0,S=255,B=0;
		int32 MinCount=6,MaxCount=6;
		if (AmbientActor != nullptr
			&& (!ReadByte(D,*AmbientActor,TEXT("AmbientHue"),H)
				|| !ReadByte(D,*AmbientActor,TEXT("AmbientSaturation"),S)
				|| !ReadByte(D,*AmbientActor,TEXT("AmbientBrightness"),B)
				|| !ReadInt(D,*AmbientActor,TEXT("MinLightCount"),MinCount)
				|| !ReadInt(D,*AmbientActor,TEXT("MaxLightCount"),MaxCount)))
		{
			Reject(*AmbientActor,TEXT("zone light-table properties are malformed"));
			H=0; S=255; B=0; MinCount=6; MaxCount=6;
		}
		Table->ZoneMinLightCounts.Add(MinCount);
		Table->ZoneMaxLightCounts.Add(MaxCount);
		FWoTZoneAmbient& Ambient=Table->ZoneAmbients.AddDefaulted_GetRef();
		Ambient.Zone=I; Ambient.Color=UE1FGetHSV(H,S,B); Ambient.bDefault=I==0;
		FVector Lo,Hi;
		if (NodeBounds(Source,I,Lo,Hi))
		{
			Ambient.BoundsMin=Lo; Ambient.BoundsMax=Hi;
		}
	}
	for (const FWoTRuntimeWorldNode& N:Source.Nodes)
	{
		Table->ZoneBspPlanes.Add(FVector4(N.Plane[0],N.Plane[1],N.Plane[2],N.Plane[3]));
		Table->ZoneBspFront.Add(N.Front >= 0 ? N.Front
			: -(static_cast<int32>(N.Zones[1]) + 1));
		Table->ZoneBspBack.Add(N.Back >= 0 ? N.Back
			: -(static_cast<int32>(N.Zones[0]) + 1));
		Table->LeafBspLeaves.Add(N.Leaves[0]); Table->LeafBspLeaves.Add(N.Leaves[1]);
		constexpr uint8 NFNotCsg = 0x01;
		constexpr uint8 NFIsNew = 0x20;
		Table->LeafBspCsg.Add(N.VertexCount > 0
			&& (N.Flags & (NFNotCsg | NFIsNew)) == 0 ? 1 : 0);
	}
	Table->bBspRootOutside=Source.bBSPRootOutside;
	for (int32 NodeIndex=0; NodeIndex<Source.Nodes.Num(); ++NodeIndex)
	{
		const FWoTRuntimeWorldNode& Node=Source.Nodes[NodeIndex];
		if (!Source.Surfaces.IsValidIndex(Node.Surface)) continue;
		const float Friction=Source.Surfaces[Node.Surface].Friction;
		if (Friction == 1.0f) continue;
		Table->FrictionNodeIndices.Add(NodeIndex);
		Table->FrictionNodeValues.Add(Friction);
	}

	TMap<FString,const FWoTRuntimeWorldActor*> ActorsByIdentity;
	for (const FWoTRuntimeWorldActor& Actor:Source.Records)
		ActorsByIdentity.Add(Actor.Identity.ToLower(),&Actor);
	struct FResolvedLightAnimation
	{
		uint8 Type = 0;
		uint8 Period = 0;
		uint8 Phase = 0;
		bool bEmits = false;
	};
	TArray<FResolvedLightAnimation> Animations;
	TMap<uint32,int32> RowBySemanticIndex;
	Table->LeafLightOffsets.Add(0);
	for (const FWoTRuntimeWorldLeaf& BSPLeaf:Source.Leaves)
	{
		if (BSPLeaf.Permeating >= 0)
		{
			for (int32 LightIndex=BSPLeaf.Permeating;
				Source.ModelLights.IsValidIndex(LightIndex)
					&& Source.ModelLights[LightIndex] != MAX_uint32; ++LightIndex)
			{
				const uint32 SemanticIndex=Source.ModelLights[LightIndex];
				if (!Source.SemanticObjects.IsValidIndex(
					static_cast<int32>(SemanticIndex)))
				{
					Error=TEXT("a leaf light is outside the semantic registry");
					return false;
				}
				int32* ExistingRow=RowBySemanticIndex.Find(SemanticIndex);
				if (ExistingRow==nullptr)
				{
					const FWoTRuntimeWorldSemanticObject& Semantic=
						Source.SemanticObjects[static_cast<int32>(SemanticIndex)];
					const FWoTRuntimeWorldActor* const* ActorPtr=
						ActorsByIdentity.Find(Semantic.Identity.ToLower());
					if (!Semantic.bActor || ActorPtr==nullptr || *ActorPtr==nullptr)
					{
						Error=TEXT("a leaf light has no decoded actor record");
						return false;
					}
					const FWoTRuntimeWorldActor& Actor=**ActorPtr;
					FRuntimeLightDefaults Defaults;
					if (!RuntimeLightDefaults(Leaf(Actor.ClassIdentity),Defaults))
					{
						Error=FString::Printf(TEXT("unsupported permeating light class '%s'"),
							*Actor.ClassIdentity);
						return false;
					}
					uint8 Type=Defaults.Type,Effect=Defaults.Effect;
					uint8 Brightness=Defaults.Brightness,Hue=Defaults.Hue;
					uint8 Saturation=Defaults.Saturation,RadiusByte=Defaults.Radius;
					uint8 Cone=Defaults.Cone,Period=0,Phase=0;
					bool bSpecialLit=false;
					FVector Position=FVector::ZeroVector;
					FIntVector Rotation=FIntVector::ZeroValue;
					FName Tag;
					if (!ReadInstanceByte(Actor,TEXT("LightType"),Type)
						|| !ReadInstanceByte(Actor,TEXT("LightEffect"),Effect)
						|| !ReadInstanceByte(Actor,TEXT("LightBrightness"),Brightness)
						|| !ReadInstanceByte(Actor,TEXT("LightHue"),Hue)
						|| !ReadInstanceByte(Actor,TEXT("LightSaturation"),Saturation)
						|| !ReadInstanceByte(Actor,TEXT("LightRadius"),RadiusByte)
						|| !ReadInstanceByte(Actor,TEXT("LightCone"),Cone)
						|| !ReadInstanceVector(Actor,TEXT("Location"),Position)
						|| !ReadInstanceRotatorUnits(Actor,TEXT("Rotation"),Rotation)
						|| !ReadInstanceName(Actor,TEXT("Tag"),Tag)
						|| !ReadBool(D,Actor,TEXT("bSpecialLit"),bSpecialLit)
						|| !ReadByte(D,Actor,TEXT("LightPeriod"),Period)
						|| !ReadByte(D,Actor,TEXT("LightPhase"),Phase))
					{
						Error=FString::Printf(TEXT("permeating light '%s' is malformed"),
							*Actor.Identity);
						return false;
					}
					Position.X=RuntimeProjectedLocation(Position.X);
					Position.Y=RuntimeProjectedLocation(Position.Y);
					Position.Z=RuntimeProjectedLocation(Position.Z);
					const float Radius=static_cast<float>(
						(Defaults.bSunLightRadius ? 250.0 : 25.0)
						* (static_cast<double>(RadiusByte)+1.0));
					const bool bEmits=Brightness != 0 && Type != 0
						&& !Leaf(Actor.ClassIdentity).Equals(
							TEXT("TriggerLight"),ESearchCase::IgnoreCase)
						&& !LightningTags.Contains(Tag);
					const double Mean=Type==2 ? 0.60 : Type==7 ? 0.90 : 1.0;
					const FVector3d HSV=UE1FGetHSVDouble(Hue,Saturation,255);
					const double Scale=(static_cast<double>(Brightness)/255.0)*Mean;
					double ColorR=HSV.X*Scale;
					double ColorG=HSV.Y*Scale;
					double ColorB=HSV.Z*Scale;
					ColorR*=Table->LevelBrightness;
					ColorG*=Table->LevelBrightness;
					ColorB*=Table->LevelBrightness;
					FWoTLightRec& Row=Table->Lights.AddDefaulted_GetRef();
					Row.Pos=Position; Row.Radius=Radius;
					Row.InvRadius=1.0f/FMath::Max(Radius,1.0f);
					Row.Color=FLinearColor(
						static_cast<float>(ColorR),static_cast<float>(ColorG),
						static_cast<float>(ColorB),1.0f);
					Row.Sine=Effect==12 ? 1.0f-static_cast<float>(Cone)/256.0f : -1.0f;
					Row.Dir=Effect==12 ? RuntimeLightDirection(Rotation) : FVector::ZeroVector;
					Row.Brightness=Brightness; Row.bSpecialLit=bSpecialLit;
					Row.bEmits=bEmits;
					const int32 RowIndex=Table->Lights.Num()-1;
					RowBySemanticIndex.Add(SemanticIndex,RowIndex);
					Animations.Add({Type,Period,Phase,bEmits});
					ExistingRow=RowBySemanticIndex.Find(SemanticIndex);
				}
				Table->LeafLightIndices.Add(*ExistingRow);
			}
		}
		Table->LeafLightOffsets.Add(Table->LeafLightIndices.Num());
	}
	TArray<uint32> WaveKeys;
	for (const FResolvedLightAnimation& Animation:Animations)
	{
		if (!Animation.bEmits
			|| (Animation.Type!=2 && Animation.Type!=3 && Animation.Type!=7)) continue;
		const uint32 Key=(static_cast<uint32>(Animation.Type)<<16)
			| (static_cast<uint32>(Animation.Period)<<8) | Animation.Phase;
		WaveKeys.AddUnique(Key);
	}
	WaveKeys.Sort();
	if (WaveKeys.Num()>4)
	{
		Error=TEXT("the runtime light table exceeds four waveform groups");
		return false;
	}
	for (int32 Index=0; Index<WaveKeys.Num(); ++Index)
	{
		const uint8 Type=static_cast<uint8>(WaveKeys[Index]>>16);
		Table->WaveMeans[Index]=UWoTDynamicLightSubsystem::RetailWaveMean(Type);
		Table->WaveAmplitudes[Index]=
			UWoTDynamicLightSubsystem::RetailWaveAmplitude(Type);
	}
	for (int32 Index=0; Index<Animations.Num(); ++Index)
	{
		const FResolvedLightAnimation& Animation=Animations[Index];
		if (!Animation.bEmits
			|| (Animation.Type!=2 && Animation.Type!=3 && Animation.Type!=7)) continue;
		const uint32 Key=(static_cast<uint32>(Animation.Type)<<16)
			| (static_cast<uint32>(Animation.Period)<<8) | Animation.Phase;
		Table->Lights[Index].WaveGroup=WaveKeys.IndexOfByKey(Key);
	}
	OutSkyAmbient=0;
	for(const FWoTRuntimeWorldActor& A:Source.Records)
	{
		if(!Leaf(A.ClassIdentity).EndsWith(
			TEXT("ZoneInfo"),ESearchCase::IgnoreCase)) continue;
		uint8 B=0;
		if(!ReadByte(D,A,TEXT("AmbientBrightness"),B))
		{
			Reject(A,TEXT("skylight ambient property is malformed"));
			continue;
		}
		if(B>0) OutSkyAmbient=OutSkyAmbient==0
			? B:FMath::Min(OutSkyAmbient,static_cast<float>(B));
	}
	Out.MapLightTable=Table; Out.bLightTablePartial=false;
	return true;
}

// Level.Title for ArenaScoreBoard.DrawHeader (uc:315-318) and
// BattleScoreBoard (uc:70). Retail reads Level == XLevel->GetLevelInfo() ==
// Actors(0): the FIRST Engine.LevelInfo record (the same record
// FillLightTable consumes). Battle2_05 also ships a stray LevelInfo2 ("The
// Inner Sanctum") that retail never reads; LevelInfo0 is "Heresy". Title is a
// presentation-only localized string: a malformed value leaves the title
// empty (the retail empty-Title branch) and never rejects the world.
void ReadLevelTitle(const FWoTWorldProjectionSource& Source,
	const FWoTRuntimeClassDefaults& D, FWoTWorldProjectionResult& Out)
{
	for (const FWoTRuntimeWorldActor& A : Source.Records)
	{
		if (!IsExact(A.ClassIdentity, TEXT("Engine.LevelInfo"))) continue;
		FString Title;
		if (ReadString(D, A, TEXT("Title"), Title)) Out.LevelTitle = MoveTemp(Title);
		return;
	}
}
}

#if WITH_AUTOMATION_TESTS
bool WoTBuildMoverMeshForTesting(AWoTMover& Mover,
	const FWoTRuntimeMoverModel& Model, FString& OutError)
{
	return BuildMoverMesh(Mover, Model,
		/*bDynamicLightMover*/ true,
		/*bHasDedicatedMoverCollision*/ false,
		/*MoverLightmap*/ nullptr, {}, {}, {},
		/*MoverBlackLightResponse*/ nullptr,
		Mover.GetGameInstance()
			? Mover.GetGameInstance()->GetSubsystem<UWoTRuntimeAssets>() : nullptr,
		OutError);
}
#endif

bool WoTApplyMoverBrushFrame(const FWoTMoverBrushFrame& Frame,
	FWoTRuntimeMoverModel& InOutModel, FString& OutError)
{
	const FVector& Main = Frame.MainScale;
	const FVector& Post = Frame.PostScale;
	if (Main.ContainsNaN() || Post.ContainsNaN() || Frame.PrePivot.ContainsNaN()
		|| Main.X == 0.0 || Main.Y == 0.0 || Main.Z == 0.0
		|| Post.X == 0.0 || Post.Y == 0.0 || Post.Z == 0.0)
	{
		OutError = TEXT("a mover brush scale is zero or non-finite");
		return false;
	}
	// PostScale acts on world axes after Rotation; the actor re-applies the
	// rotation, so conjugate it into the mesh frame. Identity PostScale (every
	// shipped MP mover but one, whose rotation is zero) skips the conjugation.
	const bool bPostScale = !Post.Equals(FVector::OneVector, 0.0);
	const FQuat Rotation = Frame.InitialRotation.Quaternion();
	auto TransformPoint = [&](const FVector3f& Local) -> FVector3f
	{
		FVector Point = (FVector(Local) - Frame.PrePivot) * Main;
		if (bPostScale)
			Point = Rotation.UnrotateVector(Rotation.RotateVector(Point) * Post);
		return FVector3f(Point);
	};
	auto TransformNormal = [&](const FVector3f& Local) -> FVector3f
	{
		FVector Normal = FVector(Local) / Main;
		if (bPostScale)
			Normal = Rotation.UnrotateVector(Rotation.RotateVector(Normal) / Post);
		return FVector3f(Normal.GetSafeNormal());
	};
	for (FVector3f& Position : InOutModel.Positions) Position = TransformPoint(Position);
	for (FVector3f& Position : InOutModel.CollisionPositions)
		Position = TransformPoint(Position);
	for (FVector3f& Normal : InOutModel.Normals) Normal = TransformNormal(Normal);
	for (FWoTRuntimeMoverSection& Section : InOutModel.Sections)
	{
		Section.PlaneOrigin = TransformPoint(Section.PlaneOrigin);
		Section.PlaneNormal = TransformNormal(Section.PlaneNormal);
	}
	// A reflected transform reverses face winding, so exchange the second and
	// third indices of every triangle (UE1's FPoly::Transform flips order too).
	const double Orientation = Main.X * Main.Y * Main.Z * Post.X * Post.Y * Post.Z;
	if (Orientation < 0.0)
	{
		if (InOutModel.Indices.Num() % 3 != 0
			|| InOutModel.CollisionIndices.Num() % 3 != 0)
		{
			OutError = TEXT("a mover render or collision index buffer is not a triangle list");
			return false;
		}
		for (int32 Index = 0; Index < InOutModel.Indices.Num(); Index += 3)
			Swap(InOutModel.Indices[Index + 1], InOutModel.Indices[Index + 2]);
		for (int32 Index = 0; Index < InOutModel.CollisionIndices.Num(); Index += 3)
			Swap(InOutModel.CollisionIndices[Index + 1],
				InOutModel.CollisionIndices[Index + 2]);
	}
	return true;
}

bool FWoTWorldProjector::ProjectDeferred(UWorld& World,
	TConstArrayView<FWoTRuntimeWorldActor> Records,
	FWoTWorldProjectionResult& Out, FString& OutError)
{
	Out=FWoTWorldProjectionResult();
	if (!IsInGameThread() || !World.PersistentLevel)
	{ OutError=TEXT("world projection requires a game-thread persistent level"); return false; }
	for (int32 I=0; I<Records.Num(); ++I)
	{
		const FWoTRuntimeWorldActor& A=Records[I];
		if (!IsExact(A.ClassIdentity,TEXT("Engine.PlayerStart"))) continue;
		FVector Location=FVector::ZeroVector; FRotator Rotation=FRotator::ZeroRotator; uint8 Team=0;
		if (const auto* P=WoTRuntimeWorldFindProperty(A,TEXT("Location"))) if(!WoTRuntimeWorldPropertyVector(*P,Location)){OutError=TEXT("a projected PlayerStart has an invalid Location");return false;}
		if (const auto* P=WoTRuntimeWorldFindProperty(A,TEXT("Rotation"))) if(!WoTRuntimeWorldPropertyRotator(*P,Rotation)){OutError=TEXT("a projected PlayerStart has an invalid Rotation");return false;}
		if (const auto* P=WoTRuntimeWorldFindProperty(A,TEXT("TeamNumber"))) if(!WoTRuntimeWorldPropertyByte(*P,Team)){OutError=TEXT("a projected PlayerStart has an invalid TeamNumber");return false;}
		AWoTPlayerStart* Start=SpawnDeferred<AWoTPlayerStart>(World,I,PlayerStartRule,FTransform(Rotation,Location));
		if(!Start){OutError=TEXT("a PlayerStart projection could not be allocated");return false;}
		Start->SetReplicates(false);Start->TeamNumber=Team;Out.DeferredActors.Add(Start);++Out.PlayerStartCount;
	}
	if (Out.PlayerStartCount==0)
	{
		APlayerStart* F=SpawnFallback<APlayerStart>(World,TEXT("WoTProjected_Fallback_PlayerStart"),FTransform(FRotator::ZeroRotator,FVector(0,0,200)));
		if(!F){OutError=TEXT("the no-starts PlayerStart fallback could not be allocated");return false;}
		F->SetReplicates(false);Out.DeferredActors.Add(F);Out.PlayerStartCount=1;Out.bUsedPlayerStartFallback=true;
	}
	return true;
}

bool FWoTWorldProjector::ProjectDeferred(UWorld& World,
	const FWoTWorldProjectionSource& Source, const FWoTRuntimeClassDefaults& D,
	FWoTWorldProjectionResult& Out, FString& OutError)
{
	Out=FWoTWorldProjectionResult();
	if (!IsInGameThread() || !World.PersistentLevel || !D.IsLoaded())
	{ OutError=TEXT("world projection requires a game-thread persistent level and class defaults"); return false; }
	TSet<FName> LightningTags;
	for (const FWoTRuntimeWorldActor& A:Source.Records)
		if (Leaf(A.ClassIdentity).Equals(TEXT("Lightning"),ESearchCase::IgnoreCase))
		{
			FName LightTag;
			if(!ReadName(D,A,TEXT("LightTag"),LightTag))
			{
				SkipActor(Out, LightRule, A.Identity,
					TEXT("a lightning light tag is malformed"));
				continue;
			}
			if(!LightTag.IsNone())LightningTags.Add(LightTag);
		}
	if (!ProjectLeafMap(World, Source, Out, OutError)) return false;
	TMap<FString, TArray<int32>> CoronaLeavesByIdentity;
	for (int32 LeafIndex = 0; LeafIndex < Source.Leaves.Num(); ++LeafIndex)
	{
		const int32 FirstLight = Source.Leaves[LeafIndex].Permeating;
		if (FirstLight < 0) continue;
		for (int32 LightIndex = FirstLight;
			Source.ModelLights.IsValidIndex(LightIndex)
				&& Source.ModelLights[LightIndex] != MAX_uint32; ++LightIndex)
		{
			const uint32 SemanticIndex = Source.ModelLights[LightIndex];
			if (!Source.SemanticObjects.IsValidIndex(
				static_cast<int32>(SemanticIndex))) continue;
			CoronaLeavesByIdentity.FindOrAdd(
				Source.SemanticObjects[SemanticIndex].Identity.ToLower()).Add(LeafIndex);
		}
	}
	TMap<FString, TArray<AActor*>> ActorsByRecord;
	for (int32 I=0; I<Source.Records.Num(); ++I)
	{
		const FWoTRuntimeWorldActor& A=Source.Records[I]; bool Handled=false;
		const FString ClassLeaf=Leaf(A.ClassIdentity);
		const int32 FirstProjectedActor = Out.DeferredActors.Num();
		if (D.ClassIsA(A.ClassIdentity,TEXT("Engine.Mover")))
		{
			if(!ProjectMover(World,Source,D,A,I,Out,OutError))
			{
				SkipActor(Out,MoverRule,A.Identity,OutError); OutError.Reset();
			}
			Handled=true;
		}
		if (D.ClassIsA(A.ClassIdentity,TEXT("Engine.PlayerStart")) || IsExact(A.ClassIdentity,TEXT("Engine.PlayerStart")))
		{
			if(!ProjectPlayerStart(World,D,A,I,Out,OutError))
			{
				SkipActor(Out, PlayerStartRule, A.Identity, OutError);
				OutError.Reset();
			}
			Handled=true;
		}
		if (!Handled && D.ClassIsA(A.ClassIdentity, TEXT("WOT.Grunt")))
		{
			if (!ProjectNPC(World,D,A,I,Out,OutError))
			{ SkipActor(Out,NPCRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("DoorTrigger"),ESearchCase::IgnoreCase))
		{
			if(!ProjectTrigger(World,D,A,I,true,Out,OutError))
			{ SkipActor(Out,DoorTriggerRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("SpecialEventTrigger"),ESearchCase::IgnoreCase))
		{
			if(!ProjectSpecialEvent(World,D,A,I,true,Out,OutError))
			{ SkipActor(Out,SpecialEventTriggerRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("Trigger"),ESearchCase::IgnoreCase))
		{
			if(!ProjectTrigger(World,D,A,I,false,Out,OutError))
			{ SkipActor(Out,TriggerRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("SpecialEvent"),ESearchCase::IgnoreCase))
		{
			if(!ProjectSpecialEvent(World,D,A,I,false,Out,OutError))
			{ SkipActor(Out,SpecialEventRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && (ClassLeaf.Equals(TEXT("SpecialEffectsTrigger"),ESearchCase::IgnoreCase)
			|| ClassLeaf.Equals(TEXT("StochasticTrigger"),ESearchCase::IgnoreCase)))
		{
			const bool bLocal=ClassLeaf.Equals(TEXT("SpecialEffectsTrigger"),ESearchCase::IgnoreCase);
			const FName Rule=bLocal?SpecialEffectsTriggerRule:StochasticTriggerRule;
			if(!ProjectRandomEventTrigger(World,D,A,I,bLocal,Out,OutError))
			{ SkipActor(Out,Rule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("TriggerLight"),ESearchCase::IgnoreCase))
		{
			if(!ProjectTriggerLight(World,D,A,I,Out,OutError))
			{ SkipActor(Out,TriggerLightRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && (ClassLeaf.Equals(TEXT("Dispatcher"),ESearchCase::IgnoreCase)
			|| ClassLeaf.Equals(TEXT("WOTDispatcher"),ESearchCase::IgnoreCase)))
		{
			if(!ProjectDispatcher(World,D,A,I,Out,OutError))
			{ SkipActor(Out,DispatcherRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("Counter"),ESearchCase::IgnoreCase))
		{
			if(!ProjectCounter(World,D,A,I,Out,OutError))
			{ SkipActor(Out,CounterRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("SoundDispatcher"),ESearchCase::IgnoreCase))
		{
			if(!ProjectSoundDispatcher(World,D,A,I,Out,OutError))
			{ SkipActor(Out,SoundDispatcherRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(
			TEXT("DynamicAmbientSound"), ESearchCase::IgnoreCase))
		{
			if (!ProjectDynamicAmbientSound(World,D,A,I,Out,OutError))
			{ SkipActor(Out,DynamicAmbientSoundRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("BattleInfo"), ESearchCase::IgnoreCase))
		{
			if (!ProjectBattleInfo(World,D,A,I,Out,OutError))
			{ SkipActor(Out,BattleInfoRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("SealAltar"), ESearchCase::IgnoreCase))
		{
			if (!ProjectSealAltar(World,D,A,I,Out,OutError))
			{ SkipActor(Out,SealAltarRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && D.ClassIsA(A.ClassIdentity, TEXT("WOT.BudgetInfo")))
		{
			if (!ProjectBudget(World,D,A,I,Out,OutError))
			{ SkipActor(Out,BudgetRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("MashadarGuide"), ESearchCase::IgnoreCase))
		{
			if (!ProjectMashadarGuide(World,D,A,I,Out,OutError))
			{ SkipActor(Out,MashadarGuideRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("Portcullis"),ESearchCase::IgnoreCase))
		{
			if(!ProjectPortcullis(World,D,A,I,Out,OutError))
			{ SkipActor(Out,PortcullisRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("PitOpening"),ESearchCase::IgnoreCase))
		{
			if(!ProjectPitOpening(World,D,A,I,Out,OutError))
			{ SkipActor(Out,PitOpeningRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("Staircase"),ESearchCase::IgnoreCase))
		{
			if(!ProjectStaircase(World,D,A,I,Out,OutError))
			{ SkipActor(Out,StaircaseRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && (ClassLeaf.Equals(TEXT("Teleporter"),ESearchCase::IgnoreCase)
			|| ClassLeaf.Equals(TEXT("TeleporterWithEffects"),ESearchCase::IgnoreCase)))
		{
			if(!ProjectTeleporter(World,D,A,I,Out,OutError))
			{ SkipActor(Out,TeleporterRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("Jumper"),ESearchCase::IgnoreCase))
		{
			if(!ProjectJumper(World,D,A,I,Out,OutError))
			{ SkipActor(Out,JumperRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("BlockAll"),ESearchCase::IgnoreCase))
		{
			if(!ProjectBlocker(World,D,A,I,EWoTCollisionBlockerMode::All,Out,OutError))
			{ SkipActor(Out,BlockAllRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("BlockPlayer"),ESearchCase::IgnoreCase))
		{
			if(!ProjectBlocker(World,D,A,I,EWoTCollisionBlockerMode::Players,Out,OutError))
			{ SkipActor(Out,BlockPlayerRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("BlockMonsters"),ESearchCase::IgnoreCase))
		{
			if(!ProjectBlocker(World,D,A,I,EWoTCollisionBlockerMode::Monsters,Out,OutError))
			{ SkipActor(Out,BlockMonstersRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("RandomLightning"),ESearchCase::IgnoreCase))
		{
			if(!ProjectLightningFX(World,D,A,I,true,Out,OutError))
			{SkipActor(Out,RandomLightningRule,A.Identity,OutError);OutError.Reset();}
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("DistanceLightning"),ESearchCase::IgnoreCase))
		{
			if(!ProjectDistanceLightning(World,D,A,I,Out,OutError))
			{SkipActor(Out,DistanceLightningRule,A.Identity,OutError);OutError.Reset();}
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("Lightning"),ESearchCase::IgnoreCase))
		{
			if(!ProjectLightningFX(World,D,A,I,false,Out,OutError))
			{SkipActor(Out,LightningRule,A.Identity,OutError);OutError.Reset();}
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("Wind"),ESearchCase::IgnoreCase)
			&& A.ClassIdentity.StartsWith(TEXT("ParticleSystems."),ESearchCase::IgnoreCase))
		{
			if(!ProjectWind(World,D,A,I,Out,OutError))
			{SkipActor(Out,WindRule,A.Identity,OutError);OutError.Reset();}
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("SpewTrigger"),ESearchCase::IgnoreCase))
		{
			if(!ProjectSpew(World,D,A,I,Out,OutError))
			{SkipActor(Out,SpewTriggerRule,A.Identity,OutError);OutError.Reset();}
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("WOTExplodingWall"),
			ESearchCase::IgnoreCase))
		{
			if(!ProjectExplodingWall(World,D,A,I,Out,OutError))
			{ SkipActor(Out,ExplodingWallRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("DestroyDirective"),
			ESearchCase::IgnoreCase))
		{
			if(!ProjectDestroyDirective(World,D,A,I,Out,OutError))
			{ SkipActor(Out,DestroyDirectiveRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (D.ClassIsA(A.ClassIdentity,TEXT("Angreal.AngrealSpawnPool")) || A.ClassIdentity.EndsWith(TEXT("AngrealSpawnPool"),ESearchCase::IgnoreCase))
		{
			if(!ProjectPool(World,D,A,I,Out,OutError))
			{
				SkipActor(Out, SpawnPoolRule, A.Identity, OutError);
				OutError.Reset();
			}
			Handled=true;
		}
		if (!Handled && D.ClassIsA(A.ClassIdentity, TEXT("Engine.Inventory"))
			&& ResolveNativePickupClass(A.ClassIdentity) != nullptr
			&& (HasEffectiveMesh(D, A)
				|| ClassLeaf.Equals(TEXT("LegionInvSeeker"), ESearchCase::IgnoreCase)))
		{
			if(!ProjectPickup(World,D,A,I,Out,OutError))
			{
				SkipActor(Out, FixedPickupRule, A.Identity, OutError);
				OutError.Reset();
			}
			Handled=true;
		}
		if (!Handled && ClassLeaf.Equals(TEXT("DecalManager"), ESearchCase::IgnoreCase))
		{
			if (!ProjectDecalManager(World,D,A,I,Out,OutError))
			{ SkipActor(Out,DecalManagerRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (!Handled
			&& D.ClassIsA(A.ClassIdentity, TEXT("Engine.Decoration"))
			&& !IsExact(A.ClassIdentity, TEXT("Environment.RainCyd")))
		{
			if(!ProjectDecoration(World,Source,D,A,I,Out,OutError))
			{
				SkipActor(Out, DecorationRule, A.Identity, OutError);
				OutError.Reset();
			}
			else if (D.ClassIsA(A.ClassIdentity, TEXT("WOT.WOTCarcass")))
			{
				// WOTCarcass.PostBeginPlay forces placed PHYS_None bodies to
				// SetCollision(bCollideActors, false, false). Keep this carcass-only
				// override outside ProjectDecoration, which is owned by the physics wave.
				for (int32 ActorIndex = FirstProjectedActor;
					ActorIndex < Out.DeferredActors.Num(); ++ActorIndex)
				{
					if (AWoTDecoration* Carcass = Cast<AWoTDecoration>(
						Out.DeferredActors[ActorIndex]))
					{
						Carcass->ConfigureRuntimeCollision(Carcass->bCollideActors,
							false, false,
							Carcass->Collision->GetUnscaledCapsuleRadius(),
							Carcass->Collision->GetUnscaledCapsuleHalfHeight());
					}
				}
			}
			Handled=true;
		}
		if (!Handled && IsExact(A.ClassIdentity, TEXT("Environment.RainCyd")))
		{
			if(!ProjectRainCyd(World,D,A,I,Out,OutError))
			{ SkipActor(Out,RainCydRule,A.Identity,OutError); OutError.Reset(); }
			Handled=true;
		}
		if (IsExact(A.ClassIdentity,TEXT("Engine.Light")))
		{
			if(!ProjectLight(World,D,A,I,Out,OutError,LightningTags))
			{
				SkipActor(Out, LightRule, A.Identity, OutError);
				OutError.Reset();
			}
			Handled=true;
		}
		if (D.ClassIsA(A.ClassIdentity,TEXT("ParticleSystems.ParticleSprayer"))
			|| IsExact(A.ClassIdentity,TEXT("ParticleSystems.ParticleSprayer")))
		{
			if(!ProjectSprayer(World,Source,D,A,I,Out,OutError))
			{
				SkipActor(Out, ParticleSprayerRule, A.Identity, OutError);
				OutError.Reset();
			}
			Handled=true;
		}
		if(IsExact(A.ClassIdentity,TEXT("Engine.ZoneInfo"))
			|| IsExact(A.ClassIdentity,TEXT("WOT.WOTZoneInfo"))
			|| IsExact(A.ClassIdentity,TEXT("WOT.WaterZone"))) Handled=true;
		if (IsPathClass(A.ClassIdentity)) Handled=true;
		if (ClassLeaf.Equals(TEXT("ActorRotator"),ESearchCase::IgnoreCase))
			Handled=true;
		if (!Handled)
		{
			if(IsInertClass(D,A.ClassIdentity))AddInert(Out,A.ClassIdentity);
			else AddUnsupported(Out,A.ClassIdentity);
		}
		for (int32 ActorIndex = FirstProjectedActor;
			ActorIndex < Out.DeferredActors.Num(); ++ActorIndex)
		{
			if (AActor* Projected = Out.DeferredActors[ActorIndex])
				ActorsByRecord.FindOrAdd(A.Identity.ToLower()).Add(Projected);
		}
		if (!ProjectAmbientSound(World,D,A,I,Out,OutError))
		{ SkipActor(Out,AmbientSoundRule,A.Identity,OutError); OutError.Reset(); }
		if (!ProjectCorona(World,D,A,I,LightningTags,
			CoronaLeavesByIdentity,Out,OutError))
		{ SkipActor(Out,CoronaRule,A.Identity,OutError); OutError.Reset(); }
	}
	ProjectActorRotators(World,Source,D,ActorsByRecord,Out);
	ProjectSoundMarkers(World,Source,D,Out);
	if (!ProjectFogLights(World,Source,D,Out,OutError)) return false;
	if (!ProjectLightWave(World,Source,D,LightningTags,Out,OutError)) return false;
	for (int32 I=0; I<Source.Zones.Num(); ++I)
		if(!ProjectZone(World,Source.Zones[I],I,Source,D,Out,OutError))
		{
			SkipActor(Out, ZoneRule, Source.Zones[I].ActorIdentity, OutError);
			OutError.Reset();
		}
	if(!ProjectPaths(World,Source,D,Out,OutError) || !ProjectNavBounds(World,Source,Out,OutError))return false;
	float Ambient=0;
	if (!FillLightTable(World,Source,D,LightningTags,Out,Ambient,OutError))
		return false;
	ReadLevelTitle(Source,D,Out);
	ASkyLight* Sky=SpawnDeferred<ASkyLight>(World,0,SkyLightRule,FTransform::Identity);
	if(!Sky){OutError=TEXT("the runtime skylight could not be allocated");return false;}
	Sky->SetReplicates(false);Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->SetIntensity(FMath::Pow(Ambient/255.f,2.2f));
	Out.DeferredActors.Add(Sky);Count(Out,SkyLightRule);
	UWoTMeshLight::RegisterRuntimeMapLightTable(&World,Out.MapLightTable);
	if(Out.PlayerStartCount==0)
	{
		APlayerStart* F=SpawnFallback<APlayerStart>(World,TEXT("WoTProjected_Fallback_PlayerStart"),FTransform(FRotator::ZeroRotator,FVector(0,0,200)));
		if(!F){OutError=TEXT("the no-starts PlayerStart fallback could not be allocated");return false;}
		F->SetReplicates(false);Out.DeferredActors.Add(F);Out.PlayerStartCount=1;Out.bUsedPlayerStartFallback=true;
	}
	Out.UnsupportedClassNames.Sort(); return true;
}
