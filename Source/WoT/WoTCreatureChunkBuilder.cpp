#include "WoTCreatureChunk.h"

#include "WoTRetailBuilderCanonical.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeStructInheritance.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTCreatureChunkRecipeSpec
{
	const TCHAR* ClassIdentity;
	int32 SlotCount;
};

// Five recipes and 62 ordered slots. Retail declarations/defaults are in
// WOTCarcass.uc:5,14-29,603-613; WOTCarcassHumanoid.uc:5,11-21;
// WOTCarcassBlack.uc:4,8-18; WOTCarcassLegion.uc:5,94-103; and
// WOTCarcassMyrddraal.uc:4,6-13.
constexpr FWoTCreatureChunkRecipeSpec GWoTCreatureChunkRecipeSpecs[] = {
	{TEXT("WOT.WOTCarcass"), 12},
	{TEXT("WOT.WOTCarcassHumanoid"), 12},
	{TEXT("WOT.WOTCarcassBlack"), 12},
	{TEXT("WoTPawns.WOTCarcassLegion"), 14},
	{TEXT("WoTPawns.WOTCarcassMyrddraal"), 12},
};

// Exact exported carcass classes whose ancestry can route a consumer lookup.
// No substring matching survives installation.
constexpr const TCHAR* GWoTCreatureChunkKnownCarcassClasses[] = {
	TEXT("WOT.WOTCarcass"),
	TEXT("WOT.WOTCarcassHumanoid"),
	TEXT("WOT.WOTCarcassBlack"),
	TEXT("WoTPawns.WOTCarcassAesSedai"),
	TEXT("WoTPawns.WOTCarcassArcher"),
	TEXT("WoTPawns.WOTCarcassBATrolloc"),
	TEXT("WoTPawns.WOTCarcassBlackAjahBoss"),
	TEXT("WoTPawns.WOTCarcassForsaken"),
	TEXT("WoTPawns.WOTCarcassHound"),
	TEXT("WoTPawns.WOTCarcassLegion"),
	TEXT("WoTPawns.WOTCarcassMinion"),
	TEXT("WoTPawns.WOTCarcassMyrddraal"),
	TEXT("WoTPawns.WOTCarcassPlayer"),
	TEXT("WoTPawns.WOTCarcassSisterSitterBA"),
	TEXT("WoTPawns.WOTCarcassSoldierQuestioner"),
	TEXT("WoTPawns.WOTCarcassTrollocMed"),
	TEXT("WoTPawns.WOTCarcassTrollocStrong"),
	TEXT("WoTPawns.WOTCarcassTrollocUber"),
	TEXT("WoTPawns.WOTCarcassTrollocWeak"),
	TEXT("WoTPawns.WOTCarcassWarder"),
	TEXT("WoTPawns.WOTCarcassWhitecloak"),
};

bool WoTCreatureChunkReadString(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, FString& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	if (Member == nullptr
		|| Member->Type != EWoTRuntimeWorldPropertyType::String)
	{
		return false;
	}
	if ((Member->Value.Num() == 1 && Member->Value[0] == 0)
		|| (Member->Value.Num() == 2 && Member->Value[0] == 1
			&& Member->Value[1] == 0))
	{
		Out.Reset();
		return true;
	}
	return WoTRuntimeWorldPropertyUE1String(*Member, Out);
}

bool WoTCreatureChunkReadInteger(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, int32& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyInteger(*Member, Out);
}

bool WoTCreatureChunkReadFloat(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, float& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyFloat(*Member, Out);
}

bool WoTCreatureChunkReadBool(const FWoTRuntimeWorldProperty& Struct,
	const TCHAR* MemberName, bool& Out)
{
	const FWoTRuntimeWorldProperty* Member =
		WoTRuntimeWorldStructMember(Struct, MemberName);
	return Member != nullptr && WoTRuntimeWorldPropertyBool(*Member, Out);
}

FName WoTCreatureChunkLeafName(const FString& Identity)
{
	int32 Dot = INDEX_NONE;
	return FName(*(Identity.FindLastChar(TEXT('.'), Dot)
		? Identity.Mid(Dot + 1) : Identity));
}

bool WoTCreatureChunkDecodePart(const FWoTRuntimeWorldProperty& Property,
	FWoTBodyPart& Out, FString& OutMember)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("BodyPartT"), ESearchCase::IgnoreCase))
	{
		OutMember = TEXT("BodyParts");
		return false;
	}
	FString MeshName;
	FString SkinName;
	FString BounceSound;
	int32 SkinSlot = 0;
	if (!WoTCreatureChunkReadString(Property, TEXT("MeshName"), MeshName))
	{
		OutMember = TEXT("MeshName"); return false;
	}
	if (!WoTCreatureChunkReadString(Property, TEXT("SkinName"), SkinName))
	{
		OutMember = TEXT("SkinName"); return false;
	}
	if (!WoTCreatureChunkReadInteger(Property, TEXT("SkinSlot"), SkinSlot))
	{
		OutMember = TEXT("SkinSlot"); return false;
	}
	if (!WoTCreatureChunkReadString(
		Property, TEXT("BounceSoundStr"), BounceSound))
	{
		OutMember = TEXT("BounceSoundStr"); return false;
	}
	if (!WoTCreatureChunkReadBool(Property, TEXT("bNoBlood"), Out.bNoBlood))
	{
		OutMember = TEXT("bNoBlood"); return false;
	}
	if (!WoTCreatureChunkReadFloat(Property, TEXT("Odds"), Out.Odds))
	{
		OutMember = TEXT("Odds"); return false;
	}
	if (!WoTCreatureChunkReadBool(Property, TEXT("bNoDamage"), Out.bNoDamage))
	{
		OutMember = TEXT("bNoDamage"); return false;
	}
	if (!WoTCreatureChunkReadBool(Property, TEXT("bZeroYaw"), Out.bZeroYaw))
	{
		OutMember = TEXT("bZeroYaw"); return false;
	}
	if (!WoTCreatureChunkReadBool(Property, TEXT("bZeroRoll"), Out.bZeroRoll))
	{
		OutMember = TEXT("bZeroRoll"); return false;
	}
	if (!WoTCreatureChunkReadBool(Property, TEXT("bZeroPitch"), Out.bZeroPitch))
	{
		OutMember = TEXT("bZeroPitch"); return false;
	}
	Out.SkinSlot = SkinSlot;

	// Generic import policy: mesh/skin strings resolve to their imported leaf,
	// while Package.Sound becomes the existing /Game/WoT/Sounds subpath form.
	Out.MeshName = MeshName.IsEmpty() ? NAME_None : WoTCreatureChunkLeafName(MeshName);
	Out.SkinTexName = SkinName.IsEmpty()
		? NAME_None : WoTCreatureChunkLeafName(SkinName);
	BounceSound.ReplaceCharInline(TEXT('.'), TEXT('/'));
	Out.BounceSound = FName(*BounceSound);
	return true;
}

bool WoTCreatureChunkBuildRecipe(const FWoTRuntimeClassDefaults& Defaults,
	const FWoTCreatureChunkRecipeSpec& Spec, TArray<FWoTBodyPart>& Out,
	FString& OutError)
{
	Out.SetNum(Spec.SlotCount);
	uint8 ChunkGoreLevel = 0;
	uint8 ReplacementGoreLevel = 0;
	const FWoTRuntimeWorldProperty* ChunkGore = Defaults.FindClassProperty(
		TEXT("WOT.WOTCreatureChunk"), TEXT("GoreLevel"));
	const FWoTRuntimeWorldProperty* ReplacementGore = Defaults.FindClassProperty(
		TEXT("WOT.WOTReplacementChunk"), TEXT("GoreLevel"));
	if (!ChunkGore || !ReplacementGore
		|| !WoTRuntimeWorldPropertyByte(*ChunkGore, ChunkGoreLevel)
		|| !WoTRuntimeWorldPropertyByte(*ReplacementGore, ReplacementGoreLevel))
	{
		OutError = TEXT("creature chunk GoreLevel defaults are missing or malformed");
		return false;
	}
	for (int32 Index = 0; Index < Spec.SlotCount; ++Index)
	{
		Out[Index].GoreLevel =
			Index == 0 ? ReplacementGoreLevel : ChunkGoreLevel;
		FWoTRuntimeWorldProperty Part;
		if (WoTRuntimeStructInheritance::Resolve(Defaults, Spec.ClassIdentity,
			TEXT("BodyParts"), static_cast<uint32>(Index), Part))
		{
			FString Member;
			if (!WoTCreatureChunkDecodePart(Part, Out[Index], Member))
			{
				OutError = FString::Printf(
					TEXT("creature chunk '%s' BodyParts[%d].%s is malformed"),
					Spec.ClassIdentity, Index, *Member);
				return false;
			}
		}
		const FWoTRuntimeWorldProperty* ZOffset = Defaults.FindClassProperty(
			Spec.ClassIdentity, TEXT("ZOffsets"), static_cast<uint32>(Index));
		if (ZOffset != nullptr
			&& !WoTRuntimeWorldPropertyFloat(*ZOffset, Out[Index].ZOffset))
		{
			OutError = FString::Printf(
				TEXT("creature chunk '%s' ZOffsets[%d] is malformed"),
				Spec.ClassIdentity, Index);
			return false;
		}
	}
	return true;
}

FName WoTCreatureChunkResolveRecipeClass(
	const FWoTRuntimeClassDefaults& Defaults, const TCHAR* ClassIdentity)
{
	// Most-specific first. Myrddraal is also a WOTCarcassBlack; Legion is a
	// base carcass with its own recipe.
	if (Defaults.ClassIsA(ClassIdentity, TEXT("WoTPawns.WOTCarcassMyrddraal")))
		return TEXT("WoTPawns.WOTCarcassMyrddraal");
	if (Defaults.ClassIsA(ClassIdentity, TEXT("WoTPawns.WOTCarcassLegion")))
		return TEXT("WoTPawns.WOTCarcassLegion");
	if (Defaults.ClassIsA(ClassIdentity, TEXT("WOT.WOTCarcassBlack")))
		return TEXT("WOT.WOTCarcassBlack");
	if (Defaults.ClassIsA(ClassIdentity, TEXT("WOT.WOTCarcassHumanoid")))
		return TEXT("WOT.WOTCarcassHumanoid");
	if (Defaults.ClassIsA(ClassIdentity, TEXT("WOT.WOTCarcass")))
		return TEXT("WOT.WOTCarcass");
	return NAME_None;
}

void WoTCreatureChunkAppendAscii(TArray<uint8>& Out, const ANSICHAR* Value)
{
	Out.Append(reinterpret_cast<const uint8*>(Value), FCStringAnsi::Strlen(Value));
}

void WoTCreatureChunkAppendInteger(TArray<uint8>& Out, int64 Value)
{
	const FString Text = FString::Printf(TEXT("%lld"), static_cast<long long>(Value));
	FTCHARToUTF8 Utf8(*Text);
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void WoTCreatureChunkAppendString(TArray<uint8>& Out, const FString& Value)
{
	FTCHARToUTF8 Utf8(*WoTRetailBuilderCanonical::Fold(Value));
	WoTCreatureChunkAppendInteger(Out, Utf8.Length());
	WoTCreatureChunkAppendAscii(Out, ":");
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

uint32 WoTCreatureChunkFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

void WoTCreatureChunkAppendFloat(TArray<uint8>& Out, float Value)
{
	constexpr ANSICHAR Digits[] = "0123456789abcdef";
	const uint32 Bits = WoTCreatureChunkFloatBits(Value);
	ANSICHAR Text[10] = {'0', 'x'};
	for (int32 Digit = 0; Digit < 8; ++Digit)
		Text[Digit + 2] = Digits[(Bits >> ((7 - Digit) * 4)) & 0x0fu];
	Out.Append(reinterpret_cast<const uint8*>(Text), UE_ARRAY_COUNT(Text));
}

template <typename ValueType>
TArray<FName> WoTCreatureChunkSortedNames(const TMap<FName, ValueType>& Map)
{
	TArray<FName> Names;
	Map.GetKeys(Names);
	Names.Sort(WoTRetailBuilderCanonical::NameLess);
	return Names;
}

bool WoTCreatureChunkFailCell(FString& OutError, FName Name,
	int32 Index, const TCHAR* Cell)
{
	OutError = FString::Printf(TEXT("creature chunk %s[%d].%s differs"),
		*Name.ToString(), Index, Cell);
	return false;
}
}

bool FWoTCreatureChunkBuilder::Build(
	const FWoTRetailBuildContext& Context, FWoTCreatureChunkSet& Out,
	FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("creature chunks require loaded class defaults");
		return false;
	}
	FWoTCreatureChunkSet Candidate;
	auto ReadDefaultFloat = [&](const TCHAR* ClassName, const TCHAR* Property,
		float& Value) -> bool
	{
		const FWoTRuntimeWorldProperty* P =
			Defaults.FindClassProperty(ClassName, Property);
		return P && WoTRuntimeWorldPropertyFloat(*P, Value);
	};
	auto ReadDefaultBool = [&](const TCHAR* ClassName, const TCHAR* Property,
		bool& Value) -> bool
	{
		const FWoTRuntimeWorldProperty* P =
			Defaults.FindClassProperty(ClassName, Property);
		return P && WoTRuntimeWorldPropertyBool(*P, Value);
	};
	FWoTCreatureChunkDefaults& D = Candidate.Defaults;
	if (!ReadDefaultFloat(TEXT("WOT.WOTCreatureChunk"), TEXT("MinBounceSpeed"), D.MinBounceSpeed)
		|| !ReadDefaultFloat(TEXT("WOT.WOTCreatureChunk"), TEXT("DeleteChunkInitialDelaySecs"), D.DeleteInitialDelay)
		|| !ReadDefaultFloat(TEXT("WOT.WOTCreatureChunk"), TEXT("DeleteChunkCheckAgainDelaySecs"), D.DeleteRecheckDelay)
		|| !ReadDefaultFloat(TEXT("WOT.WOTCreatureChunk"), TEXT("BaseSinkRatePerSecMin"), D.SinkRateMin)
		|| !ReadDefaultFloat(TEXT("WOT.WOTCreatureChunk"), TEXT("BaseSinkRatePerSecMax"), D.SinkRateMax)
		|| !ReadDefaultFloat(TEXT("WOT.WOTCreatureChunk"), TEXT("BaseInitForVelocity"), D.BaseInitVelocity)
		|| !ReadDefaultFloat(TEXT("WOT.WOTCreatureChunk"), TEXT("PainZoneDamageScale"), D.PainZoneDamageScale)
		|| !ReadDefaultBool(TEXT("WOT.WOTCreatureChunk"), TEXT("bShouldSink"), D.bShouldSink)
		|| !ReadDefaultFloat(TEXT("WOT.Gibs"), TEXT("MinBleedTime"), D.MinBleedTime)
		|| !ReadDefaultFloat(TEXT("WOT.Gibs"), TEXT("MaxBleedTime"), D.MaxBleedTime))
	{
		OutError = TEXT("creature chunk authored cleanup, sink, pain, or bleed defaults are missing or malformed");
		return false;
	}
	for (const FWoTCreatureChunkRecipeSpec& Spec : GWoTCreatureChunkRecipeSpecs)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(Spec.ClassIdentity, Chain))
		{
			OutError = FString::Printf(TEXT("creature chunk class '%s' is missing"),
				Spec.ClassIdentity);
			return false;
		}
		TArray<FWoTBodyPart> Recipe;
		if (!WoTCreatureChunkBuildRecipe(Defaults, Spec, Recipe, OutError))
			return false;
		Candidate.Recipes.Add(FName(Spec.ClassIdentity), MoveTemp(Recipe));
	}
	for (const TCHAR* ClassIdentity : GWoTCreatureChunkKnownCarcassClasses)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(ClassIdentity, Chain)) continue;
		const FName RecipeClass =
			WoTCreatureChunkResolveRecipeClass(Defaults, ClassIdentity);
		if (!RecipeClass.IsNone())
		{
			Candidate.RecipeClassByCarcassClass.Add(
				FName(ClassIdentity), RecipeClass);
		}
	}
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}

void FWoTCreatureChunkBuilder::SerializeCanonical(
	const FWoTCreatureChunkSet& Set, TArray<uint8>& Out)
{
	Out.Reset();
	WoTCreatureChunkAppendAscii(Out, "wot-creature-chunks-v2\ndefaults ");
	for (const float Value : {Set.Defaults.MinBounceSpeed,
		Set.Defaults.DeleteInitialDelay, Set.Defaults.DeleteRecheckDelay,
		Set.Defaults.SinkRateMin, Set.Defaults.SinkRateMax,
		Set.Defaults.BaseInitVelocity, Set.Defaults.PainZoneDamageScale,
		Set.Defaults.MinBleedTime, Set.Defaults.MaxBleedTime})
	{
		WoTCreatureChunkAppendFloat(Out, Value);
		WoTCreatureChunkAppendAscii(Out, " ");
	}
	WoTCreatureChunkAppendInteger(Out, Set.Defaults.bShouldSink);
	WoTCreatureChunkAppendAscii(Out, "\nrecipes ");
	const TArray<FName> Names = WoTCreatureChunkSortedNames(Set.Recipes);
	WoTCreatureChunkAppendInteger(Out, Names.Num());
	WoTCreatureChunkAppendAscii(Out, "\n");
	for (const FName Name : Names)
	{
		const TArray<FWoTBodyPart>& Parts = Set.Recipes.FindChecked(Name);
		WoTCreatureChunkAppendAscii(Out, "recipe ");
		WoTCreatureChunkAppendString(Out, Name.ToString());
		WoTCreatureChunkAppendAscii(Out, " ");
		WoTCreatureChunkAppendInteger(Out, Parts.Num());
		WoTCreatureChunkAppendAscii(Out, "\n");
		for (const FWoTBodyPart& Part : Parts)
		{
			WoTCreatureChunkAppendAscii(Out, "part ");
			WoTCreatureChunkAppendString(Out, Part.MeshName.ToString());
			WoTCreatureChunkAppendAscii(Out, " ");
			WoTCreatureChunkAppendString(Out, Part.SkinTexName.ToString());
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendInteger(Out, Part.SkinSlot);
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendFloat(Out, Part.Odds);
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendFloat(Out, Part.ZOffset);
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendInteger(Out, Part.bNoBlood);
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendInteger(Out, Part.bNoDamage);
			WoTCreatureChunkAppendAscii(Out, " ");
			WoTCreatureChunkAppendString(Out, Part.BounceSound.ToString());
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendInteger(Out, Part.bZeroYaw);
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendInteger(Out, Part.bZeroRoll);
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendInteger(Out, Part.bZeroPitch);
			WoTCreatureChunkAppendAscii(Out, " "); WoTCreatureChunkAppendInteger(Out, Part.GoreLevel);
			WoTCreatureChunkAppendAscii(Out, "\n");
		}
	}
	const TArray<FName> RouteNames =
		WoTCreatureChunkSortedNames(Set.RecipeClassByCarcassClass);
	WoTCreatureChunkAppendAscii(Out, "routes ");
	WoTCreatureChunkAppendInteger(Out, RouteNames.Num());
	WoTCreatureChunkAppendAscii(Out, "\n");
	for (const FName Name : RouteNames)
	{
		WoTCreatureChunkAppendAscii(Out, "route ");
		WoTCreatureChunkAppendString(Out, Name.ToString());
		WoTCreatureChunkAppendAscii(Out, " ");
		WoTCreatureChunkAppendString(Out,
			Set.RecipeClassByCarcassClass.FindChecked(Name).ToString());
		WoTCreatureChunkAppendAscii(Out, "\n");
	}
}

FString FWoTCreatureChunkBuilder::CanonicalDigest(
	const FWoTCreatureChunkSet& Set)
{
	TArray<uint8> Bytes;
	SerializeCanonical(Set, Bytes);
	return Sha256(Bytes).ToHex();
}

bool FWoTCreatureChunkBuilder::AreEquivalent(
	const FWoTCreatureChunkSet& Built, const FWoTCreatureChunkSet& Expected,
	FString& OutError)
{
	const FWoTCreatureChunkDefaults& DA = Built.Defaults;
	const FWoTCreatureChunkDefaults& DB = Expected.Defaults;
	auto DefaultFloatEqual = [&](const float A, const float B,
		const TCHAR* Name) -> bool
	{
		if (WoTCreatureChunkFloatBits(A) == WoTCreatureChunkFloatBits(B)) return true;
		OutError = FString::Printf(TEXT("creature chunk defaults.%s differs"), Name);
		return false;
	};
	if (!DefaultFloatEqual(DA.MinBounceSpeed, DB.MinBounceSpeed, TEXT("MinBounceSpeed"))
		|| !DefaultFloatEqual(DA.DeleteInitialDelay, DB.DeleteInitialDelay, TEXT("DeleteInitialDelay"))
		|| !DefaultFloatEqual(DA.DeleteRecheckDelay, DB.DeleteRecheckDelay, TEXT("DeleteRecheckDelay"))
		|| !DefaultFloatEqual(DA.SinkRateMin, DB.SinkRateMin, TEXT("SinkRateMin"))
		|| !DefaultFloatEqual(DA.SinkRateMax, DB.SinkRateMax, TEXT("SinkRateMax"))
		|| !DefaultFloatEqual(DA.BaseInitVelocity, DB.BaseInitVelocity, TEXT("BaseInitVelocity"))
		|| !DefaultFloatEqual(DA.PainZoneDamageScale, DB.PainZoneDamageScale, TEXT("PainZoneDamageScale"))
		|| !DefaultFloatEqual(DA.MinBleedTime, DB.MinBleedTime, TEXT("MinBleedTime"))
		|| !DefaultFloatEqual(DA.MaxBleedTime, DB.MaxBleedTime, TEXT("MaxBleedTime")))
	{
		return false;
	}
	if (DA.bShouldSink != DB.bShouldSink)
	{
		OutError = TEXT("creature chunk defaults.bShouldSink differs");
		return false;
	}
	TSet<FName> NameSet;
	for (const TPair<FName, TArray<FWoTBodyPart>>& Pair : Built.Recipes) NameSet.Add(Pair.Key);
	for (const TPair<FName, TArray<FWoTBodyPart>>& Pair : Expected.Recipes) NameSet.Add(Pair.Key);
	TArray<FName> Names = NameSet.Array();
	Names.Sort([](FName Left, FName Right)
	{
		return Left.ToString().Compare(Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (const FName Name : Names)
	{
		const TArray<FWoTBodyPart>* Left = Built.Recipes.Find(Name);
		const TArray<FWoTBodyPart>* Right = Expected.Recipes.Find(Name);
		if (Left == nullptr || Right == nullptr)
		{
			OutError = FString::Printf(TEXT("creature chunk recipe '%s' is missing from %s data"),
				*Name.ToString(), Left == nullptr ? TEXT("built") : TEXT("expected"));
			return false;
		}
		if (Left->Num() != Right->Num())
		{
			OutError = FString::Printf(TEXT("creature chunk %s row count differs: %d != %d"),
				*Name.ToString(), Left->Num(), Right->Num());
			return false;
		}
		for (int32 Index = 0; Index < Left->Num(); ++Index)
		{
			const FWoTBodyPart& A = (*Left)[Index];
			const FWoTBodyPart& B = (*Right)[Index];
			if (A.MeshName != B.MeshName) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("MeshName"));
			if (A.SkinTexName != B.SkinTexName) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("SkinTexName"));
			if (A.SkinSlot != B.SkinSlot) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("SkinSlot"));
			if (WoTCreatureChunkFloatBits(A.Odds) != WoTCreatureChunkFloatBits(B.Odds)) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("Odds"));
			if (WoTCreatureChunkFloatBits(A.ZOffset) != WoTCreatureChunkFloatBits(B.ZOffset)) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("ZOffset"));
			if (A.bNoBlood != B.bNoBlood) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("bNoBlood"));
			if (A.bNoDamage != B.bNoDamage) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("bNoDamage"));
			if (A.BounceSound != B.BounceSound) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("BounceSound"));
			if (A.bZeroYaw != B.bZeroYaw) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("bZeroYaw"));
			if (A.bZeroRoll != B.bZeroRoll) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("bZeroRoll"));
			if (A.bZeroPitch != B.bZeroPitch) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("bZeroPitch"));
			if (A.GoreLevel != B.GoreLevel) return WoTCreatureChunkFailCell(OutError, Name, Index, TEXT("GoreLevel"));
		}
	}
	TSet<FName> RouteNameSet;
	for (const TPair<FName, FName>& Pair : Built.RecipeClassByCarcassClass)
		RouteNameSet.Add(Pair.Key);
	for (const TPair<FName, FName>& Pair : Expected.RecipeClassByCarcassClass)
		RouteNameSet.Add(Pair.Key);
	TArray<FName> RouteNames = RouteNameSet.Array();
	RouteNames.Sort([](FName Left, FName Right)
	{
		return Left.ToString().Compare(
			Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (const FName CarcassClass : RouteNames)
	{
		const FName* Left = Built.RecipeClassByCarcassClass.Find(CarcassClass);
		const FName* Right = Expected.RecipeClassByCarcassClass.Find(CarcassClass);
		if (Left == nullptr || Right == nullptr)
		{
			OutError = FString::Printf(
				TEXT("creature chunk route '%s' is missing from %s data"),
				*CarcassClass.ToString(), Left == nullptr ? TEXT("built") : TEXT("expected"));
			return false;
		}
		if (*Left != *Right)
		{
			OutError = FString::Printf(
				TEXT("creature chunk route %s differs: %s != %s"),
				*CarcassClass.ToString(), *Left->ToString(), *Right->ToString());
			return false;
		}
	}
	OutError.Reset();
	return true;
}
