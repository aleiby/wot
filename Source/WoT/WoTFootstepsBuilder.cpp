#include "WoTFootsteps.h"

#include "WoTRetailBuilderCanonical.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeStructInheritance.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
constexpr const TCHAR* GWoTFootstepGenericClass = TEXT("WOT.GenericTextureHelper");
constexpr const TCHAR* GWoTFootstepLegionClass = TEXT("WoTPawns.NPCLegionTextureHelper");

struct FWoTFootstepHelperSpec
{
	const TCHAR* Key;
	const TCHAR* Identity;
	bool bNeverWalkQuiet;
	const TCHAR* OwnTable;
};

// Config defaults: GenericTextureHelper.uc:613-676 and the four authored
// helper leaves PCFemale:10-12, NPCMinion:14-17, NPCBATrolloc:14-17,
// NPCLegion:26-33. Legion's never-quiet rule is method behavior (:10-24).
constexpr FWoTFootstepHelperSpec GWoTFootstepHelpers[] = {
	{TEXT("GenericTextureHelper"), TEXT("WOT.GenericTextureHelper"), false, TEXT("")},
	{TEXT("PCFemaleTextureHelper"), TEXT("WoTPawns.PCFemaleTextureHelper"), false, TEXT("")},
	{TEXT("NPCMinionTextureHelper"), TEXT("WoTPawns.NPCMinionTextureHelper"), false, TEXT("")},
	{TEXT("NPCBATrollocTextureHelper"), TEXT("WoTPawns.NPCBATrollocTextureHelper"), false, TEXT("")},
	{TEXT("NPCLegionTextureHelper"), TEXT("WoTPawns.NPCLegionTextureHelper"), true, TEXT("Legion")},
};

// These are the nine non-generic assignments replaced by reading BOTH class
// properties below. There is no SoundTableName -> helper policy table anymore.
constexpr const TCHAR* GWoTFootstepAssignmentClasses[] = {
	TEXT("WoTPawns.AesSedai"), TEXT("WoTPawns.Sister"),
	TEXT("WoTPawns.BlackAjah"), TEXT("WoTPawns.Sitter"),
	TEXT("WoTPawns.AesSedaiBot"), TEXT("WoTPawns.BlackAjahBoss"),
	TEXT("WoTPawns.Minion"), TEXT("WoTPawns.BATrolloc"),
	TEXT("WoTPawns.Legion"),
};

FName WoTFootstepLeaf(const FString& Identity)
{
	int32 Dot = INDEX_NONE;
	return FName(*(Identity.FindLastChar(TEXT('.'), Dot)
		? Identity.RightChop(Dot + 1) : Identity));
}

FName WoTFootstepSoundTableName(const FString& Identity)
{
	FString Leaf = WoTFootstepLeaf(Identity).ToString();
	// SoundTableClass names are SoundTable<Name>, while the gameplay lookup is
	// the historical SoundTableName value (<Name>).  The class default supplies
	// that key without retaining the old pawn-name proxy map.
	if (Leaf.StartsWith(TEXT("SoundTable"), ESearchCase::CaseSensitive))
	{
		Leaf.RightChopInline(10);
	}
	return FName(*Leaf);
}

bool WoTFootstepReadFloat(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* PropertyName, float& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property =
		Defaults.FindClassProperty(Identity, PropertyName);
	if (Property == nullptr || !WoTRuntimeWorldPropertyFloat(*Property, Out))
	{
		Error = FString::Printf(TEXT("footstep helper '%s' property %s is missing or malformed"),
			Identity, PropertyName);
		return false;
	}
	return true;
}

bool WoTFootstepReadOptionalUE1String(
	const FWoTRuntimeWorldProperty& Property, FString& Out)
{
	// EffectInfoT uses empty SoundStr/EffectName cells.  The shared reader is
	// intentionally for non-empty path strings, so recognize only UE1's exact
	// empty ANSI FString encoding here and leave all other validation to it.
	if (Property.Type == EWoTRuntimeWorldPropertyType::String
		&& ((Property.Value.Num() == 1 && Property.Value[0] == 0)
			|| (Property.Value.Num() == 2
				&& Property.Value[0] == 1 && Property.Value[1] == 0)))
	{
		Out.Reset();
		return true;
	}
	return WoTRuntimeWorldPropertyUE1String(Property, Out);
}

bool WoTFootstepDecodeRow(const FWoTRuntimeWorldProperty& Property,
	FWoTFootstepRow& Out, bool& bEnd, FString& Error)
{
	bEnd = false;
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("EffectInfoT"), ESearchCase::IgnoreCase))
	{
		Error = TEXT("MovementEffects entry is not EffectInfoT"); return false;
	}
	const FWoTRuntimeWorldProperty* Group =
		WoTRuntimeWorldStructMember(Property, TEXT("Group"));
	const FWoTRuntimeWorldProperty* Surface =
		WoTRuntimeWorldStructMember(Property, TEXT("Texture"));
	const FWoTRuntimeWorldProperty* Sound =
		WoTRuntimeWorldStructMember(Property, TEXT("SoundStr"));
	const FWoTRuntimeWorldProperty* Volume =
		WoTRuntimeWorldStructMember(Property, TEXT("Volume"));
	const FWoTRuntimeWorldProperty* Pitch =
		WoTRuntimeWorldStructMember(Property, TEXT("Pitch"));
	const FWoTRuntimeWorldProperty* Effect =
		WoTRuntimeWorldStructMember(Property, TEXT("EffectName"));
	uint8 GroupValue = 0;
	uint8 SurfaceValue = 0;
	FString SoundValue;
	FString EffectValue;
	auto Require = [&Error](bool bValid, const TCHAR* Member)
	{
		if (!bValid) Error = FString(TEXT("EffectInfoT.")) + Member
			+ TEXT(" is missing or malformed");
		return bValid;
	};
	if (!Require(Group != nullptr
			&& WoTRuntimeWorldPropertyByte(*Group, GroupValue), TEXT("Group"))
		|| !Require(Surface != nullptr
			&& WoTRuntimeWorldPropertyByte(*Surface, SurfaceValue), TEXT("Texture"))
		|| !Require(Sound != nullptr
			&& WoTFootstepReadOptionalUE1String(*Sound, SoundValue), TEXT("SoundStr"))
		|| !Require(Volume != nullptr
			&& WoTRuntimeWorldPropertyFloat(*Volume, Out.Volume), TEXT("Volume"))
		|| !Require(Pitch != nullptr
			&& WoTRuntimeWorldPropertyFloat(*Pitch, Out.Pitch), TEXT("Pitch"))
		|| !Require(Effect != nullptr
			&& WoTFootstepReadOptionalUE1String(*Effect, EffectValue), TEXT("EffectName")))
	{
		return false;
	}
	if (GroupValue == 1) { bEnd = true; return true; }
	if (GroupValue == 0 || SoundValue.IsEmpty()) return true;
	if (GroupValue == 2) Out.Group = EWoTFootstepGroup::Landed;
	else if (GroupValue == 3) Out.Group = EWoTFootstepGroup::RunWalk;
	else { Error = TEXT("EffectInfoT Group is unsupported"); return false; }
	switch (SurfaceValue)
	{
	case 1: Out.Surface = EWoTSurface::Deck; break;
	case 2: Out.Surface = EWoTSurface::Dirt; break;
	case 3: Out.Surface = EWoTSurface::Grass; break;
	case 4: Out.Surface = EWoTSurface::Gravel; break;
	case 5: Out.Surface = EWoTSurface::Leaves; break;
	case 6: Out.Surface = EWoTSurface::Marble; break;
	case 7: Out.Surface = EWoTSurface::Metal; break;
	case 8: Out.Surface = EWoTSurface::Mud; break;
	case 9: Out.Surface = EWoTSurface::Puddle; break;
	case 10: Out.Surface = EWoTSurface::Rug; break;
	case 11: Out.Surface = EWoTSurface::Stone; break;
	case 12: Out.Surface = EWoTSurface::Wood; break;
	case 13: Out.Surface = EWoTSurface::All; break;
	default: Error = TEXT("EffectInfoT Texture is unsupported"); return false;
	}
	SoundValue.ReplaceInline(TEXT("."), TEXT("/"));
	Out.SoundSubPath = FName(*SoundValue);
	if (!EffectValue.IsEmpty()) Out.EffectName = WoTFootstepLeaf(EffectValue);
	return true;
}

bool WoTFootstepDecodeTable(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, TArray<FWoTFootstepRow>& Out, FString& Error)
{
	TArray<FString> Chain;
	if (!Defaults.GetClassChain(Identity, Chain))
	{
		Error = FString::Printf(TEXT("footstep table class '%s' is missing"), Identity);
		return false;
	}
	for (uint32 Index = 0; Index < 96; ++Index)
	{
		FWoTRuntimeWorldProperty Property;
		if (!WoTRuntimeStructInheritance::Resolve(Defaults, Identity,
			TEXT("MovementEffects"), Index, Property)) continue;
		FWoTFootstepRow Row;
		bool bEnd = false;
		if (!WoTFootstepDecodeRow(Property, Row, bEnd, Error))
		{
			Error = FString::Printf(TEXT("%s MovementEffects[%u]: %s"),
				Identity, Index, *Error); return false;
		}
		if (bEnd) break;
		if (!Row.SoundSubPath.IsNone()) Out.Add(MoveTemp(Row));
	}
	return true;
}

bool WoTFootstepRowEqual(const FWoTFootstepRow& A, const FWoTFootstepRow& B)
{
	using WoTRetailBuilderCanonical::FloatBits;
	return A.Group == B.Group && A.Surface == B.Surface
		&& A.SoundSubPath == B.SoundSubPath
		&& FloatBits(A.Volume) == FloatBits(B.Volume)
		&& FloatBits(A.Pitch) == FloatBits(B.Pitch)
		&& A.EffectName == B.EffectName;
}
}

bool FWoTFootstepsBuilder::Build(const FWoTRetailBuildContext& Context,
	FWoTFootstepSet& Out, FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("footsteps require loaded class defaults"); return false;
	}
	FWoTFootstepSet Candidate;
	if (!WoTFootstepDecodeTable(Defaults, GWoTFootstepGenericClass,
		Candidate.Tables.Add(TEXT("Generic")), OutError)
		|| !WoTFootstepDecodeTable(Defaults, GWoTFootstepLegionClass,
			Candidate.Tables.Add(TEXT("Legion")), OutError)) return false;
	for (const FWoTFootstepHelperSpec& Spec : GWoTFootstepHelpers)
	{
		FWoTFootstepHelper Helper;
		if (!WoTFootstepReadFloat(Defaults, Spec.Identity, TEXT("BasePitch"),
			Helper.BasePitch, OutError)
			|| !WoTFootstepReadFloat(Defaults, Spec.Identity, TEXT("BaseVolume"),
				Helper.BaseVolume, OutError)) return false;
		Helper.bNeverWalkQuiet = Spec.bNeverWalkQuiet;
		Helper.OwnTable = FName(Spec.OwnTable);
		Candidate.HelpersByClass.Add(FName(Spec.Key), Helper);
	}
	for (const TCHAR* Identity : GWoTFootstepAssignmentClasses)
	{
		const FWoTRuntimeWorldProperty* SoundTable =
			Defaults.FindClassProperty(Identity, TEXT("SoundTableClass"));
		const FWoTRuntimeWorldProperty* Helper =
			Defaults.FindClassProperty(Identity, TEXT("TextureHelperClass"));
		if (SoundTable == nullptr || Helper == nullptr
			|| SoundTable->Type != EWoTRuntimeWorldPropertyType::Object
			|| Helper->Type != EWoTRuntimeWorldPropertyType::Object)
		{
			OutError = FString::Printf(TEXT("footstep pawn '%s' helper assignment is malformed"),
				Identity); return false;
		}
		Candidate.HelperClassBySoundTable.Add(
			WoTFootstepSoundTableName(SoundTable->ObjectIdentity),
			WoTFootstepLeaf(Helper->ObjectIdentity));
	}
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}

void FWoTFootstepsBuilder::SerializeCanonical(const FWoTFootstepSet& Set,
	TArray<uint8>& Out)
{
	using namespace WoTRetailBuilderCanonical;
	Out.Reset();
	Ascii(Out, "wot-footsteps-v1\ntables "); Integer(Out, Set.Tables.Num()); Ascii(Out, "\n");
	TArray<FName> Names;
	Set.Tables.GetKeys(Names); Names.Sort(NameLess);
	for (FName TableName : Names)
	{
		const TArray<FWoTFootstepRow>& Rows = Set.Tables.FindChecked(TableName);
		Ascii(Out, "table "); Name(Out, TableName); Ascii(Out, " rows ");
		Integer(Out, Rows.Num()); Ascii(Out, "\n");
		for (const FWoTFootstepRow& Row : Rows)
		{
			Ascii(Out, "row "); Integer(Out, static_cast<uint8>(Row.Group));
			Ascii(Out, " "); Integer(Out, static_cast<uint8>(Row.Surface));
			Ascii(Out, " "); Name(Out, Row.SoundSubPath);
			Ascii(Out, " "); Float(Out, Row.Volume);
			Ascii(Out, " "); Float(Out, Row.Pitch);
			Ascii(Out, " "); Name(Out, Row.EffectName); Ascii(Out, "\n");
		}
	}
	Names.Reset(); Set.HelpersByClass.GetKeys(Names); Names.Sort(NameLess);
	Ascii(Out, "helpers "); Integer(Out, Names.Num()); Ascii(Out, "\n");
	for (FName HelperName : Names)
	{
		const FWoTFootstepHelper& H = Set.HelpersByClass.FindChecked(HelperName);
		Ascii(Out, "helper "); Name(Out, HelperName); Ascii(Out, " ");
		Float(Out, H.BasePitch); Ascii(Out, " "); Float(Out, H.BaseVolume);
		Ascii(Out, " "); Bool(Out, H.bNeverWalkQuiet); Ascii(Out, " ");
		Name(Out, H.OwnTable); Ascii(Out, "\n");
	}
	Names.Reset(); Set.HelperClassBySoundTable.GetKeys(Names); Names.Sort(NameLess);
	Ascii(Out, "assignments "); Integer(Out, Names.Num()); Ascii(Out, "\n");
	for (FName Sound : Names)
	{
		Ascii(Out, "assignment "); Name(Out, Sound); Ascii(Out, " ");
		Name(Out, Set.HelperClassBySoundTable.FindChecked(Sound)); Ascii(Out, "\n");
	}
}

FString FWoTFootstepsBuilder::CanonicalDigest(const FWoTFootstepSet& Set)
{
	TArray<uint8> Bytes; SerializeCanonical(Set, Bytes); return Sha256(Bytes).ToHex();
}

bool FWoTFootstepsBuilder::AreEquivalent(const FWoTFootstepSet& Built,
	const FWoTFootstepSet& Expected, FString& OutError)
{
	if (Built.Tables.Num() != Expected.Tables.Num()
		|| Built.HelpersByClass.Num() != Expected.HelpersByClass.Num()
		|| Built.HelperClassBySoundTable.OrderIndependentCompareEqual(
			Expected.HelperClassBySoundTable) == false)
	{
		OutError = TEXT("footstep table/helper/assignment set differs"); return false;
	}
	for (const TPair<FName, TArray<FWoTFootstepRow>>& Pair : Expected.Tables)
	{
		const TArray<FWoTFootstepRow>* Rows = Built.Tables.Find(Pair.Key);
		if (Rows == nullptr || Rows->Num() != Pair.Value.Num())
		{
			OutError = Pair.Key.ToString() + TEXT(" footstep row count differs"); return false;
		}
		for (int32 Index = 0; Index < Rows->Num(); ++Index)
		{
			if (!WoTFootstepRowEqual((*Rows)[Index], Pair.Value[Index]))
			{
				OutError = FString::Printf(TEXT("%s footstep row %d differs"),
					*Pair.Key.ToString(), Index); return false;
			}
		}
	}
	for (const TPair<FName, FWoTFootstepHelper>& Pair : Expected.HelpersByClass)
	{
		const FWoTFootstepHelper* H = Built.HelpersByClass.Find(Pair.Key);
		if (H == nullptr
			|| WoTRetailBuilderCanonical::FloatBits(H->BasePitch)
				!= WoTRetailBuilderCanonical::FloatBits(Pair.Value.BasePitch)
			|| WoTRetailBuilderCanonical::FloatBits(H->BaseVolume)
				!= WoTRetailBuilderCanonical::FloatBits(Pair.Value.BaseVolume)
			|| H->bNeverWalkQuiet != Pair.Value.bNeverWalkQuiet
			|| H->OwnTable != Pair.Value.OwnTable)
		{
			OutError = Pair.Key.ToString() + TEXT(" helper differs"); return false;
		}
	}
	OutError.Reset(); return true;
}
