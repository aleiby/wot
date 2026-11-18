#include "WoTNPCWeaponData.h"

#include "../Projectiles/WoTWeaponProjectile.h"
#include "../WoTRetailBuilderCanonical.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTNPCWeaponClassSpec
{
	const TCHAR* ClassIdentity;
};

// Retail declarations/defaults: WarderSword.uc:8,28-42;
// ArcherSword.uc:8,36-52; MyrddraalSword.uc:8,18-30;
// MyrddraalBow.uc:8,31-46; TrollocStrongAxeProxy.uc:5,7-18;
// BATHalberd.uc:4,16-31; SoldierShield.uc:8,40-57; and
// QuestionerShield.uc:8,69-94.
constexpr FWoTNPCWeaponClassSpec GWoTNPCWeaponClassSpecs[] = {
	{TEXT("WoTPawns.WarderSword")},
	{TEXT("WoTPawns.ArcherSword")},
	{TEXT("WoTPawns.MyrddraalSword")},
	{TEXT("WoTPawns.MyrddraalBow")},
	{TEXT("WoTPawns.TrollocStrongAxeProxy")},
	{TEXT("WoTPawns.BATHalberd")},
	{TEXT("WoTPawns.SoldierShield")},
	{TEXT("WoTPawns.QuestionerShield")},
};

struct FWoTNPCWeaponProjectilePolicy
{
	const TCHAR* RetailWeaponClass;
	TSubclassOf<AWoTWeaponProjectile> NativeProjectileClass;
};

const FWoTNPCWeaponProjectilePolicy* WoTNPCWeaponProjectilePolicies(
	int32& OutCount)
{
	// Explicit asset-keyed port policy retained by the classification decision.
	// Retail projectile references are at MyrddraalBow.uc:36,
	// TrollocStrongAxeProxy.uc:14, BATHalberd.uc:23, and
	// QuestionerShield.uc:80.
	static const FWoTNPCWeaponProjectilePolicy Policies[] = {
		{TEXT("WoTPawns.MyrddraalBow"), AWoTMyrddraalBolt::StaticClass()},
		{TEXT("WoTPawns.TrollocStrongAxeProxy"),
			AWoTTrollocAxeProjectile::StaticClass()},
		{TEXT("WoTPawns.BATHalberd"),
			AWoTBATHalberdProjectile::StaticClass()},
		{TEXT("WoTPawns.QuestionerShield"),
			AWoTQuestShieldProjectile::StaticClass()},
	};
	OutCount = UE_ARRAY_COUNT(Policies);
	return Policies;
}

const FWoTRuntimeWorldProperty* WoTNPCWeaponProperty(
	const FWoTRuntimeClassDefaults& Defaults, const TCHAR* ClassIdentity,
	const TCHAR* PropertyName, uint32 ArrayIndex = 0)
{
	return Defaults.FindClassProperty(ClassIdentity, PropertyName, ArrayIndex);
}

bool WoTNPCWeaponReadFloat(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* ClassIdentity, const TCHAR* PropertyName, float& Out,
	FString& OutError)
{
	const FWoTRuntimeWorldProperty* Property = WoTNPCWeaponProperty(
		Defaults, ClassIdentity, PropertyName);
	if (Property == nullptr) return true;
	if (!WoTRuntimeWorldPropertyFloat(*Property, Out))
	{
		OutError = FString::Printf(TEXT("NPC weapon '%s' property %s is malformed"),
			ClassIdentity, PropertyName);
		return false;
	}
	return true;
}

bool WoTNPCWeaponReadInteger(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* ClassIdentity, const TCHAR* PropertyName, int32& Out,
	FString& OutError)
{
	const FWoTRuntimeWorldProperty* Property = WoTNPCWeaponProperty(
		Defaults, ClassIdentity, PropertyName);
	if (Property == nullptr) return true;
	if (!WoTRuntimeWorldPropertyInteger(*Property, Out))
	{
		OutError = FString::Printf(TEXT("NPC weapon '%s' property %s is malformed"),
			ClassIdentity, PropertyName);
		return false;
	}
	return true;
}

bool WoTNPCWeaponReadBool(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* ClassIdentity, const TCHAR* PropertyName, bool& Out,
	FString& OutError)
{
	const FWoTRuntimeWorldProperty* Property = WoTNPCWeaponProperty(
		Defaults, ClassIdentity, PropertyName);
	if (Property == nullptr) return true;
	if (!WoTRuntimeWorldPropertyBool(*Property, Out))
	{
		OutError = FString::Printf(TEXT("NPC weapon '%s' property %s is malformed"),
			ClassIdentity, PropertyName);
		return false;
	}
	return true;
}

bool WoTNPCWeaponReadString(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* ClassIdentity, const TCHAR* PropertyName, FName& Out,
	FString& OutError)
{
	const FWoTRuntimeWorldProperty* Property = WoTNPCWeaponProperty(
		Defaults, ClassIdentity, PropertyName);
	if (Property == nullptr) return true;
	FString Value;
	if (!WoTRuntimeWorldPropertyUE1String(*Property, Value))
	{
		OutError = FString::Printf(TEXT("NPC weapon '%s' property %s is malformed"),
			ClassIdentity, PropertyName);
		return false;
	}
	Out = FName(*Value);
	return true;
}

bool WoTNPCWeaponReadObject(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* ClassIdentity, const TCHAR* PropertyName, FName& Out,
	bool bLeafOnly, FString& OutError, uint32 ArrayIndex = 0)
{
	const FWoTRuntimeWorldProperty* Property = WoTNPCWeaponProperty(
		Defaults, ClassIdentity, PropertyName, ArrayIndex);
	if (Property == nullptr) return true;
	if (Property->Type != EWoTRuntimeWorldPropertyType::Object)
	{
		OutError = FString::Printf(
			TEXT("NPC weapon '%s' property %s[%u] is malformed"),
			ClassIdentity, PropertyName, ArrayIndex);
		return false;
	}
	FString Identity = Property->ObjectIdentity;
	const int32 ExportMarker = Identity.Find(
		TEXT("#export-"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (ExportMarker != INDEX_NONE)
	{
		Identity.LeftInline(ExportMarker);
	}
	if (bLeafOnly)
	{
		int32 Dot = INDEX_NONE;
		if (Identity.FindLastChar(TEXT('.'), Dot)) Identity.RightChopInline(Dot + 1);
	}
	Out = FName(*Identity);
	return true;
}

bool WoTNPCWeaponDecode(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* ClassIdentity, FWoTNPCWeaponConfig& Out, FString& OutError)
{
	Out.WeaponType = FName(ClassIdentity);
	if (!WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
		TEXT("MinMeleeRange"), Out.MinMeleeRange, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("MaxMeleeRange"), Out.MaxMeleeRange, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("MeleeEffectiveness"), Out.MeleeEffectiveness, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("MinProjectileRange"), Out.MinProjectileRange, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("MaxProjectileRange"), Out.MaxProjectileRange, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("ProjectileEffectiveness"), Out.ProjectileEffectiveness, OutError)
		|| !WoTNPCWeaponReadBool(Defaults, ClassIdentity,
			TEXT("bRequireTargetVisibility"), Out.bRequireTargetVisibility, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("MissOdds"), Out.MissOdds, OutError)
		|| !WoTNPCWeaponReadObject(Defaults, ClassIdentity,
			TEXT("TypeCapableOfPreparation"), Out.TypeCapableOfPreparation,
			true, OutError)
		|| !WoTNPCWeaponReadObject(Defaults, ClassIdentity,
			TEXT("Mesh"), Out.Mesh, false, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("DrawScale"), Out.DrawScale, OutError)
		|| !WoTNPCWeaponReadInteger(Defaults, ClassIdentity,
			TEXT("Health"), Out.Health, OutError)
		|| !WoTNPCWeaponReadObject(Defaults, ClassIdentity,
			TEXT("DamageSkin0"), Out.DamageSkin0, false, OutError)
		|| !WoTNPCWeaponReadObject(Defaults, ClassIdentity,
			TEXT("DamageSkin1"), Out.DamageSkin1, false, OutError)
		|| !WoTNPCWeaponReadObject(Defaults, ClassIdentity,
			TEXT("DamageSkin2"), Out.DamageSkin2, false, OutError)
		|| !WoTNPCWeaponReadString(Defaults, ClassIdentity,
			TEXT("DestroyedSoundString"), Out.DestroyedSound, OutError)
		|| !WoTNPCWeaponReadString(Defaults, ClassIdentity,
			TEXT("TossedDecorationTypeString"), Out.TossedDecorationType, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("MPTossedLifeSpan"), Out.MPTossedLifeSpan, OutError)
		|| !WoTNPCWeaponReadFloat(Defaults, ClassIdentity,
			TEXT("ScaleGlow"), Out.ScaleGlow, OutError))
	{
		return false;
	}

	int32 HighestSkin = INDEX_NONE;
	TArray<FName> Skins;
	Skins.SetNum(8);
	for (uint32 Index = 0; Index < 8; ++Index)
	{
		if (WoTNPCWeaponProperty(Defaults, ClassIdentity,
			TEXT("MultiSkins"), Index) == nullptr) continue;
		HighestSkin = static_cast<int32>(Index);
		if (!WoTNPCWeaponReadObject(Defaults, ClassIdentity, TEXT("MultiSkins"),
			Skins[Index], false, OutError, Index)) return false;
	}
	if (HighestSkin != INDEX_NONE)
	{
		Skins.SetNum(HighestSkin + 1);
		Out.SkinOverrides = MoveTemp(Skins);
	}

	uint8 Style = 0;
	const FWoTRuntimeWorldProperty* StyleProperty =
		WoTNPCWeaponProperty(Defaults, ClassIdentity, TEXT("Style"));
	if (StyleProperty != nullptr)
	{
		if (!WoTRuntimeWorldPropertyByte(*StyleProperty, Style))
		{
			OutError = FString::Printf(
				TEXT("NPC weapon '%s' property Style is malformed"), ClassIdentity);
			return false;
		}
		Out.bStyleMasked = Style == 2; // Engine.Actor STY_Masked.
	}

	Out.bThrowable = Defaults.ClassIsA(
		ClassIdentity, TEXT("WOT.WotThrowableWeapon"));
	// QuestionerShield.uc:36-67 supplies the extra odds gate around the shared
	// WotThrowableWeapon behavior.
	Out.bQuestionerOddsGate = FStringView(ClassIdentity).Equals(
		TEXT("WoTPawns.QuestionerShield"), ESearchCase::IgnoreCase);

	int32 PolicyCount = 0;
	const FWoTNPCWeaponProjectilePolicy* Policies =
		WoTNPCWeaponProjectilePolicies(PolicyCount);
	for (int32 Index = 0; Index < PolicyCount; ++Index)
	{
		if (FStringView(ClassIdentity).Equals(
			Policies[Index].RetailWeaponClass, ESearchCase::IgnoreCase))
		{
			Out.ProjectileClass = Policies[Index].NativeProjectileClass;
			break;
		}
	}
	return true;
}

void WoTNPCWeaponAppendAscii(TArray<uint8>& Out, const ANSICHAR* Value)
{
	Out.Append(reinterpret_cast<const uint8*>(Value), FCStringAnsi::Strlen(Value));
}

void WoTNPCWeaponAppendInteger(TArray<uint8>& Out, int64 Value)
{
	const FString Text = FString::Printf(TEXT("%lld"), static_cast<long long>(Value));
	FTCHARToUTF8 Utf8(*Text);
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void WoTNPCWeaponAppendString(TArray<uint8>& Out, const FString& Value)
{
	FTCHARToUTF8 Utf8(*WoTRetailBuilderCanonical::Fold(Value));
	WoTNPCWeaponAppendInteger(Out, Utf8.Length());
	WoTNPCWeaponAppendAscii(Out, ":");
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

uint32 WoTNPCWeaponFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

void WoTNPCWeaponAppendFloat(TArray<uint8>& Out, float Value)
{
	constexpr ANSICHAR Digits[] = "0123456789abcdef";
	const uint32 Bits = WoTNPCWeaponFloatBits(Value);
	ANSICHAR Text[10] = {'0', 'x'};
	for (int32 Digit = 0; Digit < 8; ++Digit)
		Text[Digit + 2] = Digits[(Bits >> ((7 - Digit) * 4)) & 0x0fu];
	Out.Append(reinterpret_cast<const uint8*>(Text), UE_ARRAY_COUNT(Text));
}

FString WoTNPCWeaponNativeClassName(
	TSubclassOf<AWoTWeaponProjectile> ProjectileClass)
{
	return ProjectileClass.Get() == nullptr
		? FString() : ProjectileClass.Get()->GetPathName();
}

TArray<FName> WoTNPCWeaponSortedNames(const FWoTNPCWeaponSet& Set)
{
	TArray<FName> Names;
	Set.Rows.GetKeys(Names);
	Names.Sort(WoTRetailBuilderCanonical::NameLess);
	return Names;
}

bool WoTNPCWeaponFailCell(FString& OutError, FName Name, const TCHAR* Cell)
{
	OutError = FString::Printf(TEXT("NPC weapon %s.%s differs"),
		*Name.ToString(), Cell);
	return false;
}
}

bool FWoTNPCWeaponDataBuilder::Build(
	const FWoTRetailBuildContext& Context, FWoTNPCWeaponSet& Out,
	FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("NPC weapons require loaded class defaults");
		return false;
	}
	FWoTNPCWeaponSet Candidate;
	for (const FWoTNPCWeaponClassSpec& Spec : GWoTNPCWeaponClassSpecs)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(Spec.ClassIdentity, Chain))
		{
			OutError = FString::Printf(TEXT("NPC weapon class '%s' is missing"),
				Spec.ClassIdentity);
			return false;
		}
		FWoTNPCWeaponConfig Row;
		if (!WoTNPCWeaponDecode(Defaults, Spec.ClassIdentity, Row, OutError))
			return false;
		Candidate.Rows.Add(Row.WeaponType, MoveTemp(Row));
	}
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}

void FWoTNPCWeaponDataBuilder::SerializeCanonical(
	const FWoTNPCWeaponSet& Set, TArray<uint8>& Out)
{
	Out.Reset();
	WoTNPCWeaponAppendAscii(Out, "wot-npc-weapons-v1\nrows ");
	const TArray<FName> Names = WoTNPCWeaponSortedNames(Set);
	WoTNPCWeaponAppendInteger(Out, Names.Num());
	WoTNPCWeaponAppendAscii(Out, "\n");
	for (const FName Name : Names)
	{
		const FWoTNPCWeaponConfig& Row = Set.Rows.FindChecked(Name);
		WoTNPCWeaponAppendAscii(Out, "row ");
		WoTNPCWeaponAppendString(Out, Name.ToString());
#define WOT_NPC_WEAPON_CANON_FLOAT(Member) \
		WoTNPCWeaponAppendAscii(Out, " "); WoTNPCWeaponAppendFloat(Out, Row.Member)
#define WOT_NPC_WEAPON_CANON_NAME(Member) \
		WoTNPCWeaponAppendAscii(Out, " "); WoTNPCWeaponAppendString(Out, Row.Member.ToString())
#define WOT_NPC_WEAPON_CANON_INT(Member) \
		WoTNPCWeaponAppendAscii(Out, " "); WoTNPCWeaponAppendInteger(Out, Row.Member)
		WOT_NPC_WEAPON_CANON_FLOAT(MinMeleeRange);
		WOT_NPC_WEAPON_CANON_FLOAT(MaxMeleeRange);
		WOT_NPC_WEAPON_CANON_FLOAT(MeleeEffectiveness);
		WOT_NPC_WEAPON_CANON_FLOAT(MinProjectileRange);
		WOT_NPC_WEAPON_CANON_FLOAT(MaxProjectileRange);
		WOT_NPC_WEAPON_CANON_FLOAT(ProjectileEffectiveness);
		WoTNPCWeaponAppendAscii(Out, " ");
		WoTNPCWeaponAppendString(Out, WoTNPCWeaponNativeClassName(Row.ProjectileClass));
		WOT_NPC_WEAPON_CANON_INT(bRequireTargetVisibility);
		WOT_NPC_WEAPON_CANON_FLOAT(MissOdds);
		WOT_NPC_WEAPON_CANON_NAME(TypeCapableOfPreparation);
		WOT_NPC_WEAPON_CANON_NAME(Mesh);
		WOT_NPC_WEAPON_CANON_FLOAT(DrawScale);
		WoTNPCWeaponAppendAscii(Out, " "); WoTNPCWeaponAppendInteger(Out, Row.SkinOverrides.Num());
		for (const FName Skin : Row.SkinOverrides)
		{
			WoTNPCWeaponAppendAscii(Out, " ");
			WoTNPCWeaponAppendString(Out, Skin.ToString());
		}
		WOT_NPC_WEAPON_CANON_INT(bStyleMasked);
		WOT_NPC_WEAPON_CANON_FLOAT(ScaleGlow);
		WOT_NPC_WEAPON_CANON_INT(Health);
		WOT_NPC_WEAPON_CANON_NAME(DamageSkin0);
		WOT_NPC_WEAPON_CANON_NAME(DamageSkin1);
		WOT_NPC_WEAPON_CANON_NAME(DamageSkin2);
		WOT_NPC_WEAPON_CANON_NAME(DestroyedSound);
		WOT_NPC_WEAPON_CANON_NAME(TossedDecorationType);
		WOT_NPC_WEAPON_CANON_FLOAT(MPTossedLifeSpan);
		WOT_NPC_WEAPON_CANON_INT(bThrowable);
		WOT_NPC_WEAPON_CANON_INT(bQuestionerOddsGate);
		WoTNPCWeaponAppendAscii(Out, "\n");
#undef WOT_NPC_WEAPON_CANON_FLOAT
#undef WOT_NPC_WEAPON_CANON_NAME
#undef WOT_NPC_WEAPON_CANON_INT
	}
}

FString FWoTNPCWeaponDataBuilder::CanonicalDigest(const FWoTNPCWeaponSet& Set)
{
	TArray<uint8> Bytes;
	SerializeCanonical(Set, Bytes);
	return Sha256(Bytes).ToHex();
}

bool FWoTNPCWeaponDataBuilder::AreEquivalent(
	const FWoTNPCWeaponSet& Built, const FWoTNPCWeaponSet& Expected,
	FString& OutError)
{
	TSet<FName> NameSet;
	for (const TPair<FName, FWoTNPCWeaponConfig>& Pair : Built.Rows) NameSet.Add(Pair.Key);
	for (const TPair<FName, FWoTNPCWeaponConfig>& Pair : Expected.Rows) NameSet.Add(Pair.Key);
	TArray<FName> Names = NameSet.Array();
	Names.Sort([](FName Left, FName Right)
	{
		return Left.ToString().Compare(Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (const FName Name : Names)
	{
		const FWoTNPCWeaponConfig* Left = Built.Rows.Find(Name);
		const FWoTNPCWeaponConfig* Right = Expected.Rows.Find(Name);
		if (Left == nullptr || Right == nullptr)
		{
			OutError = FString::Printf(TEXT("NPC weapon '%s' is missing from %s data"),
				*Name.ToString(), Left == nullptr ? TEXT("built") : TEXT("expected"));
			return false;
		}
#define WOT_NPC_WEAPON_COMPARE_FLOAT(Member, Label) \
		if (WoTNPCWeaponFloatBits(Left->Member) != WoTNPCWeaponFloatBits(Right->Member)) return WoTNPCWeaponFailCell(OutError, Name, Label)
#define WOT_NPC_WEAPON_COMPARE_VALUE(Member, Label) \
		if (Left->Member != Right->Member) return WoTNPCWeaponFailCell(OutError, Name, Label)
		WOT_NPC_WEAPON_COMPARE_VALUE(WeaponType, TEXT("WeaponType"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(MinMeleeRange, TEXT("MinMeleeRange"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(MaxMeleeRange, TEXT("MaxMeleeRange"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(MeleeEffectiveness, TEXT("MeleeEffectiveness"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(MinProjectileRange, TEXT("MinProjectileRange"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(MaxProjectileRange, TEXT("MaxProjectileRange"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(ProjectileEffectiveness, TEXT("ProjectileEffectiveness"));
		WOT_NPC_WEAPON_COMPARE_VALUE(ProjectileClass, TEXT("ProjectileClass"));
		WOT_NPC_WEAPON_COMPARE_VALUE(bRequireTargetVisibility, TEXT("bRequireTargetVisibility"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(MissOdds, TEXT("MissOdds"));
		WOT_NPC_WEAPON_COMPARE_VALUE(TypeCapableOfPreparation, TEXT("TypeCapableOfPreparation"));
		WOT_NPC_WEAPON_COMPARE_VALUE(Mesh, TEXT("Mesh"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(DrawScale, TEXT("DrawScale"));
		WOT_NPC_WEAPON_COMPARE_VALUE(SkinOverrides, TEXT("SkinOverrides"));
		WOT_NPC_WEAPON_COMPARE_VALUE(bStyleMasked, TEXT("bStyleMasked"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(ScaleGlow, TEXT("ScaleGlow"));
		WOT_NPC_WEAPON_COMPARE_VALUE(Health, TEXT("Health"));
		WOT_NPC_WEAPON_COMPARE_VALUE(DamageSkin0, TEXT("DamageSkin0"));
		WOT_NPC_WEAPON_COMPARE_VALUE(DamageSkin1, TEXT("DamageSkin1"));
		WOT_NPC_WEAPON_COMPARE_VALUE(DamageSkin2, TEXT("DamageSkin2"));
		WOT_NPC_WEAPON_COMPARE_VALUE(DestroyedSound, TEXT("DestroyedSound"));
		WOT_NPC_WEAPON_COMPARE_VALUE(TossedDecorationType, TEXT("TossedDecorationType"));
		WOT_NPC_WEAPON_COMPARE_FLOAT(MPTossedLifeSpan, TEXT("MPTossedLifeSpan"));
		WOT_NPC_WEAPON_COMPARE_VALUE(bThrowable, TEXT("bThrowable"));
		WOT_NPC_WEAPON_COMPARE_VALUE(bQuestionerOddsGate, TEXT("bQuestionerOddsGate"));
#undef WOT_NPC_WEAPON_COMPARE_FLOAT
#undef WOT_NPC_WEAPON_COMPARE_VALUE
	}
	OutError.Reset();
	return true;
}
