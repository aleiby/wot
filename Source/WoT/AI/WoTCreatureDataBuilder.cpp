#include "WoTCreatureData.h"

#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvEarthTremor.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvRemoveCurse.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTAngrealInvShift.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTAngrealInvSoulBarb.h"
#include "../Inventory/WoTLegionInvSeeker.h"
#include "../WoTRetailBuilderCanonical.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTCreatureClassSpec
{
	const TCHAR* RowName;
	const TCHAR* Identity;
};

// Leaf default anchors: Warder.uc:449-514, Sister.uc:12-47,
// Sitter.uc:16-61, Trolloc.uc:98-133, TrollocStrong.uc:10-24,
// Myrddraal.uc:581-626, BATrolloc.uc:228-276, Minion.uc:467-516,
// MashadarFocusPawn.uc:265-281, Legion.uc:486-530, Soldier.uc:42-78,
// Archer.uc:246-283, and Questioner.uc:29-72.
constexpr FWoTCreatureClassSpec GWoTCreatureClasses[] = {
	{TEXT("Warder"), TEXT("WoTPawns.Warder")},
	{TEXT("Sister"), TEXT("WoTPawns.Sister")},
	{TEXT("Sitter"), TEXT("WoTPawns.Sitter")},
	{TEXT("Trolloc"), TEXT("WoTPawns.Trolloc")},
	{TEXT("TrollocStrong"), TEXT("WoTPawns.TrollocStrong")},
	{TEXT("Myrddraal"), TEXT("WoTPawns.Myrddraal")},
	{TEXT("BATrolloc"), TEXT("WoTPawns.BATrolloc")},
	{TEXT("Minion"), TEXT("WoTPawns.Minion")},
	{TEXT("MashadarFocusPawn"), TEXT("WoTPawns.MashadarFocusPawn")},
	{TEXT("Legion"), TEXT("WoTPawns.Legion")},
	{TEXT("Soldier"), TEXT("WoTPawns.Soldier")},
	{TEXT("Archer"), TEXT("WoTPawns.Archer")},
	{TEXT("Questioner"), TEXT("WoTPawns.Questioner")},
};

struct FWoTMeleeFunctionPolicy
{
	const TCHAR* RowName;
	const TCHAR* Function;
	const TCHAR* DamageProperty;
	float Height;
	const TCHAR* DamageType;
	bool bDescendingSequence = false;
	const TCHAR* HitSoundSlot = TEXT("WeaponMeleeHitEnemy");
	bool bAbortSwingOnMiss = false;
};

// Function semantics remain native policy: each mesh-notify function names the
// CDO damage scalar and the ported hit metadata. Sequence names and notify times
// are retail mesh facts and come only from FWoTVertexMeshSequence.
constexpr FWoTMeleeFunctionPolicy GWoTMeleeFunctionPolicies[] = {
	{TEXT("Warder"), TEXT("Attack1DamageTarget"), TEXT("Attack1Damage"), -0.5f, TEXT("Sliced"), true},
	{TEXT("Warder"), TEXT("AttackSkewDamageTarget"), TEXT("Attack2Damage"), 0.0f, TEXT("SkewRip"), false, TEXT("WeaponMeleeHitEnemy"), true},
	{TEXT("Warder"), TEXT("AttackRipDamageTarget"), TEXT("Attack2Damage"), 0.0f, TEXT("Ripped")},
	{TEXT("Warder"), TEXT("Attack3DamageTarget"), TEXT("Attack3Damage"), 0.5f, TEXT("Diced"), false, TEXT("WeaponMeleeHitEnemy"), true},
	{TEXT("Trolloc"), TEXT("AttackRunDamageTarget"), TEXT("AttackRunDamage"), 0.0f, TEXT("Cutup")},
	{TEXT("Trolloc"), TEXT("AttackRunBDamageTarget"), TEXT("AttackRunBDamage"), -0.5f, TEXT("Sliced")},
	{TEXT("TrollocStrong"), TEXT("AttackRunDamageTarget"), TEXT("AttackRunDamage"), 0.0f, TEXT("Cutup")},
	{TEXT("TrollocStrong"), TEXT("AttackRunBDamageTarget"), TEXT("AttackRunBDamage"), -0.5f, TEXT("Sliced")},
	{TEXT("Myrddraal"), TEXT("SwipeDamageTarget"), TEXT("SwipeDamage"), 0.0f, TEXT("Swiped")},
	{TEXT("Myrddraal"), TEXT("SkewerDamageTarget"), TEXT("SkewerDamage"), 0.0f, TEXT("Skewered")},
	{TEXT("BATrolloc"), TEXT("JabHalberdDamageTarget"), TEXT("JabHalberdDamage"), 0.0f, TEXT("Jabbed")},
	{TEXT("BATrolloc"), TEXT("JabHalberdLongDamageTarget"), TEXT("JabHalberdLongDamage"), 0.0f, TEXT("Skewered")},
	{TEXT("BATrolloc"), TEXT("DbleHalberdDamageTarget"), TEXT("DbleHalberdDamage"), 0.0f, TEXT("Chopped")},
	{TEXT("BATrolloc"), TEXT("DblePunchDamageTarget"), TEXT("DblePunchDamage"), 0.0f, TEXT("Punched")},
	{TEXT("BATrolloc"), TEXT("HeadTossDamageTarget"), TEXT("HeadTossDamage"), 0.0f, TEXT("Tossed")},
	{TEXT("Minion"), TEXT("AttackBiteDamage"), TEXT("AttackBiteDamage1"), 0.0f, TEXT("Bit")},
	{TEXT("Minion"), TEXT("AttackClawDamage"), TEXT("AttackClawDamage1"), 0.0f, TEXT("Clawed")},
	{TEXT("Minion"), TEXT("AttackRunClawDamage"), TEXT("AttackRunClawDamage1"), 0.0f, TEXT("Clawed")},
	{TEXT("Minion"), TEXT("TentacleWhipAttackDamage"), TEXT("AttackRunTentDamage1"), 0.5f, TEXT("Whipped")},
	{TEXT("Minion"), TEXT("TentacleGrabAttackDamage1"), TEXT("AttackTentDamage1"), 0.0f, TEXT("Grabbed")},
	{TEXT("Soldier"), TEXT("AttackRunShieldDamageTarget"), TEXT("AttackRunShieldDamage"), 0.0f, TEXT("Slammed"), false, TEXT("WeaponMeleeShieldHitEnemy")},
	{TEXT("Soldier"), TEXT("AttackRunSwipeDamageTarget"), TEXT("AttackRunSwipeDamage"), 0.0f, TEXT("Sliced")},
	{TEXT("Soldier"), TEXT("AttackRunLungeDamageTarget"), TEXT("AttackRunLungeDamage"), 0.0f, TEXT("Skewered")},
	{TEXT("Archer"), TEXT("SwipeDamageTarget"), TEXT("SwipeDamage"), 0.0f, TEXT("Sliced")},
	{TEXT("Archer"), TEXT("SkewerDamageTarget"), TEXT("SkewerDamage"), 0.0f, TEXT("Skewered")},
	{TEXT("Questioner"), TEXT("AttackRunShieldDamageTarget"), TEXT("AttackRunShieldDamage"), 0.0f, TEXT("Slammed"), false, TEXT("WeaponMeleeShieldHitEnemy")},
	{TEXT("Questioner"), TEXT("AttackRunSwipeDamageTarget"), TEXT("AttackRunSwipeDamage"), 0.0f, TEXT("Sliced")},
	{TEXT("Questioner"), TEXT("AttackRunLungeDamageTarget"), TEXT("AttackRunLungeDamage"), 0.0f, TEXT("Skewered")},
};

struct FWoTCreatureBehaviorPolicy
{
	const TCHAR* RowName;
	const TCHAR* Behavior;
};

// Seven exact leaf dispatches plus the generic ShieldUser rule below.
constexpr FWoTCreatureBehaviorPolicy GWoTCreatureBehaviorPolicies[] = {
	{TEXT("Warder"), TEXT("WarderFadeCloak")},
	{TEXT("Myrddraal"), TEXT("MyrddraalTeleport")},
	{TEXT("BATrolloc"), TEXT("BATrollocRecovery")},
	{TEXT("Minion"), TEXT("MinionStealth")},
	{TEXT("MashadarFocusPawn"), TEXT("MashadarTendrilFocus")},
	{TEXT("Legion"), TEXT("LegionSpiritsStomp")},
	{TEXT("Archer"), TEXT("ArcherBow")},
};

struct FWoTWeaveBinding
{
	const TCHAR* Leaf;
	UClass* (*Class)();
};

template <typename T> UClass* WoTCreatureNativeClass() { return T::StaticClass(); }

const FWoTWeaveBinding GWoTWeaveBindings[] = {
	{TEXT("AngrealInvAirBurst"), &WoTCreatureNativeClass<AWoTAngrealInvAirBurst>},
	{TEXT("AngrealInvDart"), &WoTCreatureNativeClass<AWoTAngrealInvDart>},
	{TEXT("AngrealInvSeeker"), &WoTCreatureNativeClass<AWoTAngrealInvSeeker>},
	{TEXT("AngrealInvShift"), &WoTCreatureNativeClass<AWoTAngrealInvShift>},
	{TEXT("AngrealInvShield"), &WoTCreatureNativeClass<AWoTAngrealInvShield>},
	{TEXT("AngrealInvEarthTremor"), &WoTCreatureNativeClass<AWoTAngrealInvEarthTremor>},
	{TEXT("AngrealInvFireball"), &WoTCreatureNativeClass<AWoTAngrealInvFireball>},
	{TEXT("AngrealInvSoulBarb"), &WoTCreatureNativeClass<AWoTAngrealInvSoulBarb>},
	{TEXT("AngrealInvEarthShield"), &WoTCreatureNativeClass<AWoTAngrealInvEarthShield>},
	{TEXT("AngrealInvRemoveCurse"), &WoTCreatureNativeClass<AWoTAngrealInvRemoveCurse>},
	{TEXT("AngrealInvFireShield"), &WoTCreatureNativeClass<AWoTAngrealInvFireShield>},
	{TEXT("LegionInvSeeker"), &WoTCreatureNativeClass<AWoTLegionInvSeeker>},
};

FString WoTCreatureLeafString(const FString& Identity)
{
	FString Public = Identity;
	const int32 Marker = Public.Find(TEXT("#export-"), ESearchCase::IgnoreCase,
		ESearchDir::FromEnd);
	if (Marker > 0) Public.LeftInline(Marker);
	int32 Dot = INDEX_NONE;
	return Public.FindLastChar(TEXT('.'), Dot) ? Public.RightChop(Dot + 1) : Public;
}

FString WoTCreaturePublicIdentity(const FString& Identity)
{
	const int32 Marker = Identity.Find(TEXT("#export-"),
		ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	return Marker > 0 ? Identity.Left(Marker) : Identity;
}

FName WoTCreatureLeaf(const FString& Identity)
{
	return FName(*WoTCreatureLeafString(Identity));
}

bool WoTCreatureReadFloat(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, float& Out, FString& Error,
	uint32 Index = 0, bool bRequired = false)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name, Index);
	if (Property == nullptr)
	{
		if (!bRequired) return true;
		Error = FString::Printf(TEXT("creature '%s' property %s[%u] is missing"),
			Identity, Name, Index); return false;
	}
	if (!WoTRuntimeWorldPropertyFloat(*Property, Out))
	{
		Error = FString::Printf(TEXT("creature '%s' property %s[%u] is malformed"),
			Identity, Name, Index); return false;
	}
	return true;
}

bool WoTCreatureReadInteger(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, int32& Out, FString& Error,
	uint32 Index = 0, bool bRequired = false)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name, Index);
	if (Property == nullptr)
	{
		if (!bRequired) return true;
		Error = FString::Printf(TEXT("creature '%s' property %s[%u] is missing"),
			Identity, Name, Index); return false;
	}
	if (!WoTRuntimeWorldPropertyInteger(*Property, Out))
	{
		Error = FString::Printf(TEXT("creature '%s' property %s[%u] is malformed"),
			Identity, Name, Index); return false;
	}
	return true;
}

bool WoTCreatureReadNumberAsFloat(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, float& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property =
		Defaults.FindClassProperty(Identity, Name);
	if (Property == nullptr) return true;
	if (WoTRuntimeWorldPropertyFloat(*Property, Out)) return true;
	int32 Integer = 0;
	if (WoTRuntimeWorldPropertyInteger(*Property, Integer))
	{
		Out = static_cast<float>(Integer);
		return true;
	}
	Error = FString::Printf(TEXT("creature '%s' property %s[0] is malformed"),
		Identity, Name);
	return false;
}

bool WoTCreatureReadByte(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, uint8& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name);
	if (Property == nullptr) return true;
	if (!WoTRuntimeWorldPropertyByte(*Property, Out))
	{
		Error = FString::Printf(TEXT("creature '%s' property %s is malformed"), Identity, Name);
		return false;
	}
	return true;
}

bool WoTCreatureReadBool(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, bool& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name);
	if (Property == nullptr) return true;
	if (!WoTRuntimeWorldPropertyBool(*Property, Out))
	{
		Error = FString::Printf(TEXT("creature '%s' property %s is malformed"), Identity, Name);
		return false;
	}
	return true;
}

bool WoTCreatureReadObject(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, FName& Out, FString& Error,
	bool bLeaf = false, uint32 Index = 0)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name, Index);
	if (Property == nullptr) return true;
	if (Property->Type != EWoTRuntimeWorldPropertyType::Object)
	{
		Error = FString::Printf(TEXT("creature '%s' property %s[%u] is malformed"),
			Identity, Name, Index); return false;
	}
	Out = bLeaf ? WoTCreatureLeaf(Property->ObjectIdentity)
		: FName(*WoTCreaturePublicIdentity(Property->ObjectIdentity));
	return true;
}

UClass* WoTCreatureBoundWeave(FName SourcePath)
{
	const FString Leaf = WoTCreatureLeafString(SourcePath.ToString());
	for (const FWoTWeaveBinding& Binding : GWoTWeaveBindings)
	{
		if (Leaf.Equals(Binding.Leaf, ESearchCase::IgnoreCase)) return Binding.Class();
	}
	return nullptr;
}

bool WoTCreatureDecodeWeaves(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, FWoTCreatureRow& Row, FString& Error)
{
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		const FWoTRuntimeWorldProperty* Path = Defaults.FindClassProperty(
			Identity, TEXT("AII_AngrealInventoryStr"), Index);
		if (Path == nullptr) continue;
		FString Source;
		if (!WoTRuntimeWorldPropertyUE1String(*Path, Source))
		{
			Error = FString::Printf(TEXT("creature '%s' AII path %u is malformed"),
				Identity, Index); return false;
		}
		if (Source.IsEmpty()) continue;
		FWoTCreatureWeave& Weave = Row.Weaves.AddDefaulted_GetRef();
		Weave.SourcePath = FName(*Source);
		Weave.WeaveClass = WoTCreatureBoundWeave(Weave.SourcePath);
		if (!Weave.WeaveClass)
		{
			Error = FString::Printf(TEXT("creature '%s' weave '%s' has no native binding"),
				Identity, *Source); return false;
		}
		if (!WoTCreatureReadFloat(Defaults, Identity, TEXT("AII_Priority"),
			Weave.Priority, Error, Index)
			|| !WoTCreatureReadInteger(Defaults, Identity,
				TEXT("AII_MinChargesInGroup"), Weave.MinChargesInGroup, Error, Index)
			|| !WoTCreatureReadInteger(Defaults, Identity,
				TEXT("AII_MaxChargesInGroup"), Weave.MaxChargesInGroup, Error, Index)
			|| !WoTCreatureReadFloat(Defaults, Identity,
				TEXT("AII_MinChargeGroupInterval"),
				Weave.MinChargeGroupInterval, Error, Index)) return false;
	}
	return true;
}

bool WoTCreatureDecodeDodge(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, FWoTCreatureRow& Row, FString& Error)
{
	const FWoTRuntimeWorldProperty* Detector = Defaults.FindClassProperty(
		Identity, TEXT("DurationNotifierClasses"), 3);
	if (Detector == nullptr || Detector->Type != EWoTRuntimeWorldPropertyType::Object
		|| Detector->ObjectIdentity.IsEmpty()) return true;
	const FString& DetectorClass = Detector->ObjectIdentity;
	bool bActive = false;
	if (!WoTCreatureReadBool(Defaults, *DetectorClass, TEXT("bActiveDetection"),
		bActive, Error) || !bActive) return Error.IsEmpty();
	const FWoTRuntimeWorldProperty* Response = Defaults.FindClassProperty(
		DetectorClass, TEXT("DefensiveResponses"), 0);
	if (Response == nullptr || Response->Type != EWoTRuntimeWorldPropertyType::Struct)
	{
		Error = FString::Printf(TEXT("defensive detector '%s' response is missing"),
			*DetectorClass); return false;
	}
	const FWoTRuntimeWorldProperty* Min =
		WoTRuntimeWorldStructMember(*Response, TEXT("DR_MinResponseTime"));
	const FWoTRuntimeWorldProperty* Max =
		WoTRuntimeWorldStructMember(*Response, TEXT("DR_MaxResponseTime"));
	const FWoTRuntimeWorldProperty* Hint =
		WoTRuntimeWorldStructMember(*Response, TEXT("DR_ResponsePreHint"));
	float ResponseMin = 0.0f;
	float ResponseMax = 0.0f;
	if (Min == nullptr || Max == nullptr || Hint == nullptr
		|| !WoTRuntimeWorldPropertyFloat(*Min, ResponseMin)
		|| !WoTRuntimeWorldPropertyFloat(*Max, ResponseMax)
		|| Hint->Type != EWoTRuntimeWorldPropertyType::Name)
	{
		Error = FString::Printf(TEXT("defensive detector '%s' response is malformed"),
			*DetectorClass); return false;
	}
	Row.bDefensiveDodge = Hint->NameValue.Equals(TEXT("AttemptDodge"),
		ESearchCase::IgnoreCase);
	if (!Row.bDefensiveDodge) return true;
	Row.DodgeResponseMinTime = ResponseMin;
	Row.DodgeResponseMaxTime = ResponseMax;
	return WoTCreatureReadFloat(Defaults, *DetectorClass,
		TEXT("CollectionRadius"), Row.DodgeCollectionRadius, Error);
}

bool WoTCreatureDecodeMelee(const FWoTRetailBuildContext& Context,
	const FWoTCreatureClassSpec& Spec, FWoTCreatureRow& Row, FString& Error)
{
	struct FNotifyOccurrence
	{
		const FWoTVertexMeshSequence* Sequence = nullptr;
		const FWoTVertexMeshNotify* Notify = nullptr;
		int32 Ordinal = 0;
	};
	TArray<FWoTVertexMeshSequence> Sequences;
	const FString MeshIdentity = Row.Mesh.ToString();
	if (!Context.FindMeshSequences
		|| !Context.FindMeshSequences(MeshIdentity, Sequences))
	{
		Error = FString::Printf(
			TEXT("creature '%s' mesh '%s' is unavailable"),
			Spec.Identity, *MeshIdentity);
		return false;
	}
	// The family has a published canonical row order independent of package
	// export order. Function policy supplies the group order; sequence names and
	// notify times still come exclusively from the decoded mesh. Warder's two
	// Attack1 sequences retain the legacy public row order (swipe, lunge).
	for (const FWoTMeleeFunctionPolicy& Policy : GWoTMeleeFunctionPolicies)
	{
		if (!Row.RowName.IsEqual(Policy.RowName)) continue;
		TArray<FNotifyOccurrence> Occurrences;
		int32 Ordinal = 0;
		for (const FWoTVertexMeshSequence& Sequence : Sequences)
		{
			for (const FWoTVertexMeshNotify& Notify : Sequence.Notifies)
			{
				if (Notify.Function.Equals(
					Policy.Function, ESearchCase::IgnoreCase))
				{
					Occurrences.Add({&Sequence, &Notify, Ordinal});
				}
				++Ordinal;
			}
		}
		Occurrences.Sort([&Policy](const FNotifyOccurrence& Left,
			const FNotifyOccurrence& Right)
		{
			const int32 SequenceOrder = Left.Sequence->Name.Compare(
				Right.Sequence->Name, ESearchCase::IgnoreCase);
			if (SequenceOrder != 0)
			{
				return Policy.bDescendingSequence
					? SequenceOrder > 0 : SequenceOrder < 0;
			}
			if (Left.Notify->Time != Right.Notify->Time)
			{
				return Left.Notify->Time < Right.Notify->Time;
			}
			return Left.Ordinal < Right.Ordinal;
		});
		for (const FNotifyOccurrence& Occurrence : Occurrences)
		{
			FWoTMeleeAttack Attack;
			if (!WoTCreatureReadFloat(Context.ClassDefaults, Spec.Identity,
				Policy.DamageProperty, Attack.Damage, Error, 0, true))
			{
				return false;
			}
			Attack.HeightModifier = Policy.Height;
			Attack.DamageTypeName = Policy.DamageType;
			Attack.AnimName = FName(*Occurrence.Sequence->Name);
			Attack.NotifyFraction = Occurrence.Notify->Time;
			Attack.HitSoundSlot = Policy.HitSoundSlot;
			Attack.bAbortSwingOnMiss = Policy.bAbortSwingOnMiss;
			Row.MeleeAttacks.Add(MoveTemp(Attack));
		}
	}
	return true;
}

bool WoTCreatureDecodeRow(const FWoTRetailBuildContext& Context,
	const FWoTCreatureClassSpec& Spec, FWoTCreatureRow& Row, FString& Error)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	TArray<FString> Chain;
	if (!Defaults.GetClassChain(Spec.Identity, Chain))
	{
		Error = FString::Printf(TEXT("creature class '%s' is missing"), Spec.Identity);
		return false;
	}
	Row.RowName = Spec.RowName;
	if (!WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("HealthSPMin"), Row.HealthSPMin, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("HealthSPMax"), Row.HealthSPMax, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("HealthMPMin"), Row.HealthMPMin, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("HealthMPMax"), Row.HealthMPMax, Error)
		|| !WoTCreatureReadInteger(Defaults, Spec.Identity, TEXT("Health"), Row.HealthPlaced, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("Mass"), Row.Mass, Error)
		|| !WoTCreatureReadNumberAsFloat(Defaults, Spec.Identity,
			TEXT("BrokenMoralePct"), Row.BrokenMoralePct, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("GibForSureFinalHealth"), Row.GibForSureFinalHealth, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("GibSometimesFinalHealth"), Row.GibSometimesFinalHealth, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("ChanceOfHitAnim"), Row.ChanceOfHitAnim, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("HitSoftHealthRatio"), Row.HitSoftHealthRatio, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("GroundSpeed"), Row.GroundSpeed, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("GroundSpeedMin"), Row.GroundSpeedMin, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("GroundSpeedMax"), Row.GroundSpeedMax, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("BaseWalkingSpeed"), Row.BaseWalkingSpeed, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("WaterSpeed"), Row.WaterSpeed, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("GoalSuggestedSpeeds"), Row.ChaseSuggestedSpeed, Error, 0)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("GoalSuggestedSpeeds"), Row.WaitSuggestedSpeed, Error, 6)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("MeleeRange"), Row.MeleeRange, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("MinOffensiveAngrealUsageInterval"), Row.MinOffensiveAngrealUsageInterval, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("MinDefensiveAngrealUsageInterval"), Row.MinDefensiveAngrealUsageInterval, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("DrawScale"), Row.DrawScale, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("ScaleGlow"), Row.ScaleGlow, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("CollisionRadius"), Row.CollisionRadius, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("CollisionHeight"), Row.CollisionHeight, Error)
		|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("BaseEyeHeight"), Row.BaseEyeHeight, Error)
		|| !WoTCreatureReadBool(Defaults, Spec.Identity, TEXT("bIsFemale"), Row.bIsFemale, Error)
		|| !WoTCreatureReadBool(Defaults, Spec.Identity, TEXT("bBlockActors"), Row.bBlockActors, Error)
		|| !WoTCreatureReadBool(Defaults, Spec.Identity, TEXT("bBlockPlayers"), Row.bBlockPlayers, Error)
		|| !WoTCreatureReadBool(Defaults, Spec.Identity, TEXT("bProjTarget"), Row.bProjTarget, Error)
		|| !WoTCreatureReadByte(Defaults, Spec.Identity, TEXT("Visibility"), Row.Visibility, Error)
		|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("DisguiseIcon"), Row.DisguiseIcon, Error)
		|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("DefaultWeaponType"), Row.DefaultWeaponType, Error)
		|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("MeleeWeaponType"), Row.MeleeWeaponType, Error)
		|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("RangedWeaponType"), Row.RangedWeaponType, Error)
		|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("HandlerFactoryClass"), Row.HandlerFactoryClass, Error)
		|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("Mesh"), Row.Mesh, Error)
		|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("SoundTableClass"), Row.SoundTableClass, Error)
		|| !WoTCreatureDecodeWeaves(Defaults, Spec.Identity, Row, Error)
		|| !WoTCreatureDecodeDodge(Defaults, Spec.Identity, Row, Error)
		|| !WoTCreatureDecodeMelee(Context, Spec, Row, Error)) return false;
	auto ReadOwnObject = [&](const TCHAR* Name, FName& Value)
	{
		const FWoTRuntimeWorldProperty* Property =
			Defaults.FindOwnClassProperty(Spec.Identity, Name);
		if (Property == nullptr) return true;
		if (Property->Type != EWoTRuntimeWorldPropertyType::Object)
		{
			Error = FString::Printf(TEXT("creature '%s' own property %s is malformed"),
				Spec.Identity, Name);
			return false;
		}
		Value = FName(*WoTCreaturePublicIdentity(Property->ObjectIdentity));
		return true;
	};
	if (!ReadOwnObject(TEXT("CarcassType"), Row.CarcassType)) return false;
	for (int32 Slot = 0; Slot < 8; ++Slot)
	{
		const FWoTRuntimeWorldProperty* Skin = Defaults.FindClassProperty(
			Spec.Identity, TEXT("MultiSkins"), static_cast<uint32>(Slot));
		if (!Skin) continue;
		if (Skin->Type != EWoTRuntimeWorldPropertyType::Object)
		{
			Error = FString::Printf(TEXT("creature '%s' MultiSkins[%d] is malformed"),
				Spec.Identity, Slot);
			return false;
		}
		if (!Skin->ObjectIdentity.IsEmpty())
			Row.MaterialSkinIdentities.Add(Slot,
				FName(*WoTCreaturePublicIdentity(Skin->ObjectIdentity)));
	}
	if (Row.RowName == TEXT("Minion"))
	{
		if (!WoTCreatureReadBool(Defaults, Spec.Identity, TEXT("bEnableEyes"),
				Row.bMinionEyesEnabled, Error)
			|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("MinEyeOffTime"),
				Row.MinionEyeOffMin, Error, 0, true)
			|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("MaxEyeOffTime"),
				Row.MinionEyeOffMax, Error, 0, true)
			|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("MinEyeOnTime"),
				Row.MinionEyeOnMin, Error, 0, true)
			|| !WoTCreatureReadFloat(Defaults, Spec.Identity, TEXT("MaxEyeOnTime"),
				Row.MinionEyeOnMax, Error, 0, true)
			|| !WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("EyesTexture"),
				Row.MinionEyesTexture, Error))
		{
			return false;
		}
	}
	if (const FWoTRuntimeWorldProperty* Fov =
		Defaults.FindOwnClassProperty(Spec.Identity, TEXT("FovAngle")))
	{
		if (!WoTRuntimeWorldPropertyFloat(*Fov, Row.FovAngle))
		{
			Error = FString::Printf(TEXT("creature '%s' own FovAngle is malformed"),
				Spec.Identity);
			return false;
		}
	}
	if (const FWoTRuntimeWorldProperty* Radius =
		Defaults.FindOwnClassProperty(Spec.Identity, TEXT("SoundRadius")))
	{
		if (!WoTRuntimeWorldPropertyByte(*Radius, Row.AmbientSoundRadius))
		{
			Error = FString::Printf(TEXT("creature '%s' own SoundRadius is malformed"),
				Spec.Identity);
			return false;
		}
	}

	FName AnimationTable;
	if (!WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("AnimationTableClass"),
		AnimationTable, Error, true)) return false;
	FString AnimationName = AnimationTable.ToString();
	AnimationName.RemoveFromStart(TEXT("AnimationTable"));
	Row.AnimTable = FName(*AnimationName);
	if (!Row.MeleeWeaponType.IsNone() && Row.RowName != TEXT("Legion"))
	{
		Row.MeleeAnimSlot = TEXT("MeleeWeaponAttack");
	}
	Row.bIsCaptain = Defaults.ClassIsA(Spec.Identity, TEXT("WOT.Captain"));
	Row.bChampionOrders = Defaults.ClassIsA(Spec.Identity, TEXT("WOT.Champion"));
	Row.bShieldUser = Defaults.ClassIsA(Spec.Identity, TEXT("WoTPawns.ShieldUser"));
	for (const FWoTCreatureBehaviorPolicy& Policy : GWoTCreatureBehaviorPolicies)
	{
		if (Row.RowName.IsEqual(Policy.RowName)) Row.BehaviorClass = Policy.Behavior;
	}
	if (Row.BehaviorClass.IsNone() && Row.bShieldUser)
	{
		Row.BehaviorClass = TEXT("ShieldDeflect");
	}
	FName DamageHelper;
	if (!WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("DamageHelperClass"),
		DamageHelper, Error, true)) return false;
	Row.bBlackBlood = DamageHelper == TEXT("DamageHelperBlackBlood");
	FName Ambient;
	if (!ReadOwnObject(TEXT("AmbientSound"), Ambient)) return false;
	if (!Ambient.IsNone())
	{
		FString Path = Ambient.ToString(); Path.ReplaceInline(TEXT("."), TEXT("/"));
		Row.AmbientSoundPath = FName(*Path);
	}
	// Only Legion authors a creature-specific turn rate.  The inherited Engine
	// Pawn CDO value (Yaw=50000) is already represented by CharacterMovement and
	// is deliberately not an override in the registry row.
	const FWoTRuntimeWorldProperty* Rotation =
		Defaults.FindOwnClassProperty(Spec.Identity, TEXT("RotationRate"));
	if (Rotation != nullptr)
	{
		FRotator Value;
		if (!WoTRuntimeWorldPropertyRotator(*Rotation, Value))
		{
			Error = FString::Printf(TEXT("creature '%s' RotationRate is malformed"), Spec.Identity);
			return false;
		}
		Row.RotationRateYaw = FMath::RoundToInt(Value.Yaw * (65536.0f / 360.0f));
	}
	const FWoTRuntimeWorldProperty* Initial = Defaults.FindClassProperty(
		Spec.Identity, TEXT("InitialState"));
	if (Initial != nullptr && Initial->Type == EWoTRuntimeWorldPropertyType::Name
		&& Initial->NameValue.Equals(TEXT("Dormant"), ESearchCase::IgnoreCase))
	{
		Row.InitialState = EWoTBrainState::Dormant;
	}
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		FName Inventory;
		if (!WoTCreatureReadObject(Defaults, Spec.Identity, TEXT("DefaultInventoryTypes"),
			Inventory, Error, false, Index)) return false;
		if (!Inventory.IsNone()) Row.DefaultInventoryTypes.Add(Inventory);
	}
	if (Row.RowName == TEXT("Legion"))
	{
		Row.bExcludesLegionProjectileTarget = true;
		const FString Adapter = Row.RangedWeaponType.ToString();
		if (!WoTCreatureReadFloat(Defaults, *Adapter,
				TEXT("MinOffensiveUsageInterval"),
				Row.MinOffensiveAngrealUsageInterval, Error)
			|| !WoTCreatureReadFloat(Defaults, *Adapter,
				TEXT("MinProjectileRange"), Row.AdapterMinProjectileRange, Error)
			|| !WoTCreatureReadFloat(Defaults, *Adapter,
				TEXT("MaxProjectileRange"), Row.AdapterMaxProjectileRange, Error)
			|| !WoTCreatureReadBool(Defaults, *Adapter,
				TEXT("bRequireTargetVisibility"),
				Row.bAdapterRequireTargetVisibility, Error)) return false;
		Row.HoldRange = 3200.0f;
		FWoTCreatureWeave& Weave = Row.Weaves.AddDefaulted_GetRef();
		Weave.SourcePath = TEXT("WoTPawns.LegionInvSeeker");
		Weave.WeaveClass = AWoTLegionInvSeeker::StaticClass();
		Weave.Priority = 1.0f;
	}
	else if (Row.Weaves.Num() > 0)
	{
		// RangeHandlerFactory data is a separate family; 400 is the existing
		// port-policy standoff until that builder lands.
		Row.HoldRange = 400.0f;
	}
	return true;
}

bool WoTCreatureDecodeTroops(const FWoTRuntimeClassDefaults& Defaults,
	FWoTCreatureSet& Out, FString& Error)
{
	for (uint32 Index = 0; Index < 12; ++Index)
	{
		const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(
			TEXT("WOT.WOTPlayer"), TEXT("PlayerTroops"), Index);
		if (Property == nullptr || Property->Type != EWoTRuntimeWorldPropertyType::Struct)
		{
			Error = FString::Printf(TEXT("WOTPlayer PlayerTroops[%u] is missing or malformed"), Index);
			return false;
		}
		const FWoTRuntimeWorldProperty* Player =
			WoTRuntimeWorldStructMember(*Property, TEXT("Player"));
		const FWoTRuntimeWorldProperty* Troop =
			WoTRuntimeWorldStructMember(*Property, TEXT("Troop"));
		const FWoTRuntimeWorldProperty* Class =
			WoTRuntimeWorldStructMember(*Property, TEXT("TroopClassName"));
		FString ClassName;
		if (Player == nullptr || Troop == nullptr || Class == nullptr
			|| Player->Type != EWoTRuntimeWorldPropertyType::Name
			|| Troop->Type != EWoTRuntimeWorldPropertyType::Name
			|| !WoTRuntimeWorldPropertyUE1String(*Class, ClassName))
		{
			Error = FString::Printf(TEXT("WOTPlayer PlayerTroops[%u] members are malformed"), Index);
			return false;
		}
		FWoTPlayerTroopRow& Mapping = Out.PlayerTroops.AddDefaulted_GetRef();
		Mapping.Faction = FName(*Player->NameValue);
		Mapping.Tier = FName(*Troop->NameValue);
		Mapping.RowName = WoTCreatureLeaf(ClassName);
		FWoTCreatureRow* Row = Out.Rows.Find(Mapping.RowName);
		if (Row == nullptr)
		{
			Error = FString::Printf(TEXT("PlayerTroops[%u] references unknown creature '%s'"),
				Index, *Mapping.RowName.ToString()); return false;
		}
		if (Mapping.Faction == TEXT("AesSedai")) Row->Faction = EWoTCreatureFaction::AesSedai;
		else if (Mapping.Faction == TEXT("Forsaken")) Row->Faction = EWoTCreatureFaction::Forsaken;
		else if (Mapping.Faction == TEXT("Whitecloak")) Row->Faction = EWoTCreatureFaction::Whitecloak;
		else if (Mapping.Faction == TEXT("Hound")) Row->Faction = EWoTCreatureFaction::Hound;
		else { Error = TEXT("PlayerTroops has unknown faction"); return false; }
		if (Mapping.Tier == TEXT("Grunt")) Row->Tier = EWoTCreatureTier::Grunt;
		else if (Mapping.Tier == TEXT("Captain")) Row->Tier = EWoTCreatureTier::Captain;
		else if (Mapping.Tier == TEXT("Champion")) Row->Tier = EWoTCreatureTier::Champion;
		else { Error = TEXT("PlayerTroops has unknown tier"); return false; }
	}
	FWoTCreatureRow* Strong = Out.Rows.Find(TEXT("TrollocStrong"));
	const FWoTCreatureRow* Trolloc = Out.Rows.Find(TEXT("Trolloc"));
	if (Strong == nullptr || Trolloc == nullptr)
	{
		Error = TEXT("Trolloc rows are missing"); return false;
	}
	Strong->Faction = Trolloc->Faction;
	Strong->Tier = Trolloc->Tier;
	return true;
}

void WoTCreatureSerializeAttack(TArray<uint8>& Out, const FWoTMeleeAttack& Attack)
{
	using namespace WoTRetailBuilderCanonical;
	Float(Out, Attack.Damage); Ascii(Out, " "); Float(Out, Attack.HeightModifier);
	Ascii(Out, " "); Name(Out, Attack.DamageTypeName); Ascii(Out, " ");
	Name(Out, Attack.AnimName); Ascii(Out, " "); Float(Out, Attack.NotifyFraction);
	Ascii(Out, " "); Name(Out, Attack.HitSoundSlot); Ascii(Out, " ");
	Bool(Out, Attack.bAbortSwingOnMiss);
}

void WoTCreatureSerializeWeave(TArray<uint8>& Out, const FWoTCreatureWeave& Weave)
{
	using namespace WoTRetailBuilderCanonical;
	Name(Out, Weave.SourcePath); Ascii(Out, " ");
	String(Out, Weave.WeaveClass ? Weave.WeaveClass->GetPathName() : FString(TEXT("None")));
	Ascii(Out, " "); Float(Out, Weave.Priority); Ascii(Out, " ");
	Integer(Out, Weave.MinChargesInGroup); Ascii(Out, " ");
	Integer(Out, Weave.MaxChargesInGroup); Ascii(Out, " ");
	Float(Out, Weave.MinChargeGroupInterval);
}

void WoTCreatureSerializeRow(TArray<uint8>& Out, const FWoTCreatureRow& R)
{
	using namespace WoTRetailBuilderCanonical;
	Name(Out, R.RowName); Ascii(Out, " "); Integer(Out, static_cast<uint8>(R.Tier));
	Ascii(Out, " "); Integer(Out, static_cast<uint8>(R.Faction)); Ascii(Out, " ");
	Bool(Out, R.bIsFemale); Ascii(Out, " "); Name(Out, R.DisguiseIcon);
	for (float Value : {R.HealthSPMin, R.HealthSPMax, R.HealthMPMin, R.HealthMPMax})
	{ Ascii(Out, " "); Float(Out, Value); }
	Ascii(Out, " "); Integer(Out, R.HealthPlaced);
	for (float Value : {R.Mass, R.BrokenMoralePct, R.GibForSureFinalHealth,
		R.GibSometimesFinalHealth, R.ChanceOfHitAnim, R.HitSoftHealthRatio,
		R.GroundSpeed, R.GroundSpeedMin, R.GroundSpeedMax, R.BaseWalkingSpeed,
		R.WaterSpeed, R.ChaseSuggestedSpeed, R.WaitSuggestedSpeed})
	{ Ascii(Out, " "); Float(Out, Value); }
	Ascii(Out, " "); Integer(Out, R.RotationRateYaw); Ascii(Out, " ");
	Float(Out, R.MeleeRange); Ascii(Out, " attacks "); Integer(Out, R.MeleeAttacks.Num());
	for (const FWoTMeleeAttack& Attack : R.MeleeAttacks)
	{ Ascii(Out, " ["); WoTCreatureSerializeAttack(Out, Attack); Ascii(Out, "]"); }
	for (FName Value : {R.AnimTable, R.MeleeAnimSlot, R.DefaultWeaponType,
		R.MeleeWeaponType, R.RangedWeaponType, R.HandlerFactoryClass, R.BehaviorClass})
	{ Ascii(Out, " "); Name(Out, Value); }
	Ascii(Out, " "); Bool(Out, R.bDefensiveDodge);
	for (float Value : {R.DodgeResponseMinTime, R.DodgeResponseMaxTime,
		R.DodgeCollectionRadius}) { Ascii(Out, " "); Float(Out, Value); }
	Ascii(Out, " inventory "); Integer(Out, R.DefaultInventoryTypes.Num());
	for (FName Value : R.DefaultInventoryTypes) { Ascii(Out, " "); Name(Out, Value); }
	Ascii(Out, " "); Bool(Out, R.bExcludesLegionProjectileTarget);
	Ascii(Out, " weaves "); Integer(Out, R.Weaves.Num());
	for (const FWoTCreatureWeave& Weave : R.Weaves)
	{ Ascii(Out, " ["); WoTCreatureSerializeWeave(Out, Weave); Ascii(Out, "]"); }
	for (float Value : {R.MinOffensiveAngrealUsageInterval,
		R.MinDefensiveAngrealUsageInterval, R.AdapterMinProjectileRange,
		R.AdapterMaxProjectileRange}) { Ascii(Out, " "); Float(Out, Value); }
	Ascii(Out, " "); Bool(Out, R.bAdapterRequireTargetVisibility);
	Ascii(Out, " "); Float(Out, R.HoldRange); Ascii(Out, " "); Bool(Out, R.bIsCaptain);
	Ascii(Out, " "); Integer(Out, static_cast<uint8>(R.AssignedOrders));
	Ascii(Out, " "); Bool(Out, R.bChampionOrders); Ascii(Out, " ");
	Integer(Out, static_cast<uint8>(R.InitialState));
	for (FName Value : {R.Mesh, R.CarcassType, R.SoundTableClass, R.AmbientSoundPath})
	{ Ascii(Out, " "); Name(Out, Value); }
	for (float Value : {R.DrawScale, R.ScaleGlow, R.CollisionRadius,
		R.CollisionHeight, R.BaseEyeHeight, R.FovAngle})
	{ Ascii(Out, " "); Float(Out, Value); }
	for (bool Value : {R.bBlockActors, R.bBlockPlayers, R.bProjTarget})
	{ Ascii(Out, " "); Bool(Out, Value); }
	Ascii(Out, " "); Integer(Out, R.Visibility);
	Ascii(Out, " "); Integer(Out, R.AmbientSoundRadius);
	Ascii(Out, " "); Integer(Out, R.AmbientSoundVolume);
	Ascii(Out, " "); Integer(Out, R.AmbientSoundPitch);
	Ascii(Out, " "); Bool(Out, R.bShieldUser); Ascii(Out, " "); Bool(Out, R.bBlackBlood);
	TArray<int32> SkinSlots;
	R.MaterialSkinIdentities.GetKeys(SkinSlots);
	SkinSlots.Sort();
	Ascii(Out, " skins "); Integer(Out, SkinSlots.Num());
	for (const int32 Slot : SkinSlots)
	{
		Ascii(Out, " "); Integer(Out, Slot); Ascii(Out, ":");
		Name(Out, R.MaterialSkinIdentities.FindChecked(Slot));
	}
	Ascii(Out, " eyes ");
	Name(Out, R.MinionEyesTexture);
	for (const float Value : {R.MinionEyeOffMin, R.MinionEyeOffMax,
		R.MinionEyeOnMin, R.MinionEyeOnMax})
	{ Ascii(Out, " "); Float(Out, Value); }
	Ascii(Out, " "); Bool(Out, R.bMinionEyesEnabled);
	Ascii(Out, " movement-bindings "); Integer(Out, R.MovementBindings.Num());
	for (const auto& Binding : R.MovementBindings) Binding.Serialize(Out);
}
}

bool FWoTCreatureDataBuilder::Build(const FWoTRetailBuildContext& Context,
	FWoTCreatureSet& Out, FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("creature-data requires loaded class defaults"); return false;
	}
	FWoTCreatureSet Candidate;
	TArray<FString> PatternClasses;
	Defaults.GetDerivedClassIdentities(TEXT("Legend.MovementPattern"), PatternClasses);
	for (const FString& Identity : PatternClasses)
	{
		FWoTMovementPattern Pattern;
		if (!FWoTMovementPattern::Decode(Defaults, Identity, Pattern, OutError)) return false;
		Candidate.MovementPatterns.Add(Pattern.ClassIdentity, MoveTemp(Pattern));
	}
	const FWoTRuntimeWorldProperty* TracerSpin = Defaults.FindClassProperty(
		TEXT("Angreal.AngrealTracerProjectile"), TEXT("SpinRate"));
	FRotator TracerSpinValue;
	if (!TracerSpin || !WoTRuntimeWorldPropertyRotator(*TracerSpin, TracerSpinValue))
	{
		OutError = TEXT("AngrealTracerProjectile SpinRate is missing or malformed");
		return false;
	}
	// WoTRuntimeWorldPropertyRotator has already converted UE1's 65536-unit
	// revolution to degrees.  The runtime field is degrees/sec^2 because
	// AddActorLocalRotation consumes degrees.
	Candidate.TracerSpinAccelerationRollDegPerSec2 = TracerSpinValue.Roll;
	for (const FWoTCreatureClassSpec& Spec : GWoTCreatureClasses)
	{
		FWoTCreatureRow Row;
		if (!WoTCreatureDecodeRow(Context, Spec, Row, OutError)) return false;
		for (int32 Handler : {21, 23, 1})
		{
			FWoTMovementBinding Binding;
			if (!FWoTMovementBinding::Decode(Defaults, Row.HandlerFactoryClass.ToString(),
				Handler, Handler == 1, Binding, OutError)) return false;
			if (!Binding.Pattern.ClassIdentity.IsNone()) Row.MovementBindings.Add(MoveTemp(Binding));
		}
		Candidate.Rows.Add(Row.RowName, MoveTemp(Row));
	}
	if (!WoTCreatureDecodeTroops(Defaults, Candidate, OutError)) return false;
	Out = MoveTemp(Candidate); OutError.Reset(); return true;
}

void FWoTCreatureDataBuilder::SerializeCanonical(const FWoTCreatureSet& Set,
	TArray<uint8>& Out)
{
	using namespace WoTRetailBuilderCanonical;
	Out.Reset(); Ascii(Out, "wot-creature-data-v3\nmovement-patterns ");
	TArray<FName> Patterns; Set.MovementPatterns.GetKeys(Patterns); Patterns.Sort(NameLess);
	Integer(Out, Patterns.Num());
	for (FName Identity : Patterns) Set.MovementPatterns.FindChecked(Identity).Serialize(Out);
	Ascii(Out, "\ntracer-spin ");
	Float(Out, Set.TracerSpinAccelerationRollDegPerSec2);
	Ascii(Out, "\nrows ");
	Integer(Out, Set.Rows.Num()); Ascii(Out, "\n");
	TArray<FName> Names; Set.Rows.GetKeys(Names); Names.Sort(NameLess);
	for (FName RowName : Names)
	{
		Ascii(Out, "row "); WoTCreatureSerializeRow(Out, Set.Rows.FindChecked(RowName));
		Ascii(Out, "\n");
	}
	Ascii(Out, "troops "); Integer(Out, Set.PlayerTroops.Num()); Ascii(Out, "\n");
	for (const FWoTPlayerTroopRow& Mapping : Set.PlayerTroops)
	{
		Ascii(Out, "troop "); Name(Out, Mapping.Faction); Ascii(Out, " ");
		Name(Out, Mapping.Tier); Ascii(Out, " "); Name(Out, Mapping.RowName);
		Ascii(Out, "\n");
	}
}

FString FWoTCreatureDataBuilder::CanonicalDigest(const FWoTCreatureSet& Set)
{
	TArray<uint8> Bytes; SerializeCanonical(Set, Bytes); return Sha256(Bytes).ToHex();
}

bool FWoTCreatureDataBuilder::AreEquivalent(const FWoTCreatureSet& Built,
	const FWoTCreatureSet& Expected, FString& OutError)
{
	FWoTCreatureSet BuiltPatterns, ExpectedPatterns;
	BuiltPatterns.MovementPatterns = Built.MovementPatterns;
	ExpectedPatterns.MovementPatterns = Expected.MovementPatterns;
	TArray<uint8> BuiltPatternBytes, ExpectedPatternBytes;
	SerializeCanonical(BuiltPatterns, BuiltPatternBytes); SerializeCanonical(ExpectedPatterns, ExpectedPatternBytes);
	if (BuiltPatternBytes != ExpectedPatternBytes)
	{ OutError = TEXT("movement pattern definitions differ"); return false; }
	if (Built.Rows.Num() != Expected.Rows.Num())
	{
		OutError = TEXT("creature row count differs"); return false;
	}
	if (WoTRetailBuilderCanonical::FloatBits(
		Built.TracerSpinAccelerationRollDegPerSec2)
		!= WoTRetailBuilderCanonical::FloatBits(
			Expected.TracerSpinAccelerationRollDegPerSec2))
	{
		OutError = TEXT("tracer spin acceleration differs");
		return false;
	}
	for (const TPair<FName, FWoTCreatureRow>& Pair : Expected.Rows)
	{
		const FWoTCreatureRow* Actual = Built.Rows.Find(Pair.Key);
		if (Actual == nullptr)
		{
			OutError = Pair.Key.ToString() + TEXT(" creature row is missing"); return false;
		}
		FWoTCreatureSet Left; Left.Rows.Add(Pair.Key, *Actual);
		FWoTCreatureSet Right; Right.Rows.Add(Pair.Key, Pair.Value);
		TArray<uint8> LeftBytes; TArray<uint8> RightBytes;
		SerializeCanonical(Left, LeftBytes); SerializeCanonical(Right, RightBytes);
		if (LeftBytes != RightBytes)
		{
			int32 Offset = 0;
			const int32 Common = FMath::Min(LeftBytes.Num(), RightBytes.Num());
			while (Offset < Common && LeftBytes[Offset] == RightBytes[Offset]) ++Offset;
			auto Snippet = [Offset](const TArray<uint8>& Bytes)
			{
				const int32 Begin = FMath::Max(0, Offset - 32);
				const int32 Count = FMath::Min(96, Bytes.Num() - Begin);
				if (Count <= 0) return FString();
				const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(
					Bytes.GetData() + Begin), Count);
				return FString(Converted.Length(), Converted.Get());
			};
			OutError = FString::Printf(TEXT("%s creature row differs at canonical byte %d; "
				"built='%s' expected='%s'"), *Pair.Key.ToString(), Offset,
				*Snippet(LeftBytes), *Snippet(RightBytes));
			return false;
		}
	}
	if (Built.PlayerTroops.Num() != Expected.PlayerTroops.Num())
	{
		OutError = TEXT("player troop count differs"); return false;
	}
	for (int32 Index = 0; Index < Built.PlayerTroops.Num(); ++Index)
	{
		const FWoTPlayerTroopRow& A = Built.PlayerTroops[Index];
		const FWoTPlayerTroopRow& B = Expected.PlayerTroops[Index];
		if (A.Faction != B.Faction || A.Tier != B.Tier || A.RowName != B.RowName)
		{
			OutError = FString::Printf(TEXT("player troop %d differs"), Index); return false;
		}
	}
	OutError.Reset(); return true;
}
