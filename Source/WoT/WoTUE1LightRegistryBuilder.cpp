#include "WoTUE1LightRegistry.h"

#include "WoTRetailBuilderCanonical.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTUE1LightClassSpec
{
	const TCHAR* RowName;
	const TCHAR* ClassIdentity;
};

// The scalar defaults are cited at the corresponding exported files and lines
// in WoTUE1LightRegistry.cpp. The package-qualified class declarations are at
// each file's class line (normally :12; WOT/Explosion.uc:30).
constexpr FWoTUE1LightClassSpec GWoTUE1LightClassSpecs[] = {
	{TEXT("AngrealFireballProjectile"), TEXT("Angreal.AngrealFireballProjectile")},
	{TEXT("AngrealDartProjectile"), TEXT("Angreal.AngrealDartProjectile")},
	{TEXT("AngrealDecayProjectile"), TEXT("Angreal.AngrealDecayProjectile")},
	{TEXT("AngrealEarthTremorProjectile"), TEXT("Angreal.AngrealEarthTremorProjectile")},
	{TEXT("AngrealExpWardProjectile"), TEXT("Angreal.AngrealExpWardProjectile")},
	{TEXT("AngrealIceProjectile"), TEXT("Angreal.AngrealIceProjectile")},
	{TEXT("AngrealSeekerProjectile"), TEXT("Angreal.AngrealSeekerProjectile")},
	{TEXT("AngrealSoulBarbProjectile"), TEXT("Angreal.AngrealSoulBarbProjectile")},
	{TEXT("AngrealWallOfAirProjectile"), TEXT("Angreal.AngrealWallOfAirProjectile")},
	{TEXT("Explosion"), TEXT("WOT.Explosion")},
	{TEXT("DartExplosion"), TEXT("Angreal.DartExplosion")},
	{TEXT("LSImpact"), TEXT("Angreal.LSImpact")},
	{TEXT("SoulBarbImpact"), TEXT("WOT.SoulBarbImpact")},
	{TEXT("BurningChunk"), TEXT("Angreal.BurningChunk")},
	{TEXT("BFExpSprayer"), TEXT("Angreal.BFExpSprayer")},
	{TEXT("LightningSkinEffect"), TEXT("Angreal.LightningSkinEffect")},
	{TEXT("ReflectSkinEffect"), TEXT("Angreal.ReflectSkinEffect")},
	{TEXT("ShieldParticleMesh"), TEXT("Angreal.ShieldParticleMesh")},
	{TEXT("TaintVisual"), TEXT("Angreal.TaintVisual")},
	{TEXT("TaintExpTorus"), TEXT("Angreal.TaintExpTorus")},
	{TEXT("WhirlwindLeech"), TEXT("Angreal.WhirlwindLeech")},
	{TEXT("AMAPearl"), TEXT("Angreal.AMAPearl")},
	{TEXT("PixieSprayer"), TEXT("Angreal.PixieSprayer")},
	{TEXT("DeflectSprayer"), TEXT("Angreal.DeflectSprayer")},
	{TEXT("BalefireCorona"), TEXT("Angreal.BalefireCorona")},
	{TEXT("ETSoundProxy"), TEXT("Angreal.ETSoundProxy")},
};

bool WoTUE1LightReadOptionalByte(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* ClassIdentity, const TCHAR* PropertyName, uint8& Out,
	FString& OutError)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(
		ClassIdentity, PropertyName);
	if (Property == nullptr) return true;
	if (!WoTRuntimeWorldPropertyByte(*Property, Out))
	{
		OutError = FString::Printf(TEXT("UE1 light '%s' property %s is malformed"),
			ClassIdentity, PropertyName);
		return false;
	}
	return true;
}

bool WoTUE1LightDecodeShape(uint8 Value, EWoTUE1LightShape& Out)
{
	switch (Value)
	{
	case 0: Out = EWoTUE1LightShape::None; return true;
	case 13: Out = EWoTUE1LightShape::NonIncidence; return true;
	case 14: Out = EWoTUE1LightShape::Shell; return true;
	default: return false;
	}
}

bool WoTUE1LightDecodeType(uint8 Value, EWoTDynLightType& Out)
{
	switch (Value)
	{
	case 1: Out = EWoTDynLightType::Steady; return true;
	case 2: Out = EWoTDynLightType::Pulse; return true;
	case 3: Out = EWoTDynLightType::Blink; return true;
	case 4: Out = EWoTDynLightType::Flicker; return true;
	case 7: Out = EWoTDynLightType::SubtlePulse; return true;
	default: return false;
	}
}

void WoTUE1LightAppendAscii(TArray<uint8>& Out, const ANSICHAR* Value)
{
	Out.Append(reinterpret_cast<const uint8*>(Value), FCStringAnsi::Strlen(Value));
}

void WoTUE1LightAppendInteger(TArray<uint8>& Out, int64 Value)
{
	const FString Text = FString::Printf(TEXT("%lld"),
		static_cast<long long>(Value));
	FTCHARToUTF8 Utf8(*Text);
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void WoTUE1LightAppendString(TArray<uint8>& Out, const FString& Value)
{
	FTCHARToUTF8 Utf8(*WoTRetailBuilderCanonical::Fold(Value));
	WoTUE1LightAppendInteger(Out, Utf8.Length());
	WoTUE1LightAppendAscii(Out, ":");
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

uint32 WoTUE1LightFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

void WoTUE1LightAppendFloat(TArray<uint8>& Out, float Value)
{
	constexpr ANSICHAR Digits[] = "0123456789abcdef";
	const uint32 Bits = WoTUE1LightFloatBits(Value);
	ANSICHAR Text[10] = {'0', 'x'};
	for (int32 Digit = 0; Digit < 8; ++Digit)
	{
		Text[Digit + 2] = Digits[(Bits >> ((7 - Digit) * 4)) & 0x0fu];
	}
	Out.Append(reinterpret_cast<const uint8*>(Text), UE_ARRAY_COUNT(Text));
}

TArray<FName> WoTUE1LightSortedNames(const FWoTUE1LightSet& Set)
{
	TArray<FName> Names;
	Set.Rows.GetKeys(Names);
	Names.Sort(WoTRetailBuilderCanonical::NameLess);
	return Names;
}

bool WoTUE1LightFailCell(FString& OutError, FName Name, const TCHAR* Cell)
{
	OutError = FString::Printf(TEXT("UE1 light %s.%s differs"),
		*Name.ToString(), Cell);
	return false;
}
}

bool FWoTUE1LightRegistryBuilder::Build(
	const FWoTRetailBuildContext& Context, FWoTUE1LightSet& Out,
	FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("UE1 lights require loaded class defaults");
		return false;
	}
	FWoTUE1LightSet Candidate;
	for (const FWoTUE1LightClassSpec& Spec : GWoTUE1LightClassSpecs)
	{
		TArray<FString> Chain;
		if (!Defaults.GetClassChain(Spec.ClassIdentity, Chain))
		{
			OutError = FString::Printf(TEXT("UE1 light class '%s' is missing"),
				Spec.ClassIdentity);
			return false;
		}
		uint8 Radius = 0;
		uint8 Shape = 0;
		uint8 Type = 1;
		FWoTUE1LightRow Row;
		if (!WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
			TEXT("LightBrightness"), Row.Brightness, OutError)
			|| !WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
				TEXT("LightHue"), Row.Hue, OutError)
			|| !WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
				TEXT("LightSaturation"), Row.Saturation, OutError)
			|| !WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
				TEXT("LightRadius"), Radius, OutError)
			|| !WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
				TEXT("LightEffect"), Shape, OutError)
			|| !WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
				TEXT("LightType"), Type, OutError)
			|| !WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
				TEXT("LightPeriod"), Row.Period, OutError)
			|| !WoTUE1LightReadOptionalByte(Defaults, Spec.ClassIdentity,
				TEXT("LightPhase"), Row.Phase, OutError))
		{
			return false;
		}
		if (!WoTUE1LightDecodeShape(Shape, Row.Shape)
			|| !WoTUE1LightDecodeType(Type, Row.Type))
		{
			OutError = FString::Printf(
				TEXT("UE1 light '%s' has unsupported light enum values"),
				Spec.ClassIdentity);
			return false;
		}
		Row.Radius = static_cast<float>(Radius);
		Candidate.Rows.Add(FName(Spec.RowName), Row);
	}
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}

void FWoTUE1LightRegistryBuilder::SerializeCanonical(
	const FWoTUE1LightSet& Set, TArray<uint8>& Out)
{
	Out.Reset();
	WoTUE1LightAppendAscii(Out, "wot-ue1-lights-v1\nrows ");
	const TArray<FName> Names = WoTUE1LightSortedNames(Set);
	WoTUE1LightAppendInteger(Out, Names.Num());
	WoTUE1LightAppendAscii(Out, "\n");
	for (const FName Name : Names)
	{
		const FWoTUE1LightRow& Row = Set.Rows.FindChecked(Name);
		WoTUE1LightAppendAscii(Out, "row ");
		WoTUE1LightAppendString(Out, Name.ToString());
		WoTUE1LightAppendAscii(Out, " ");
		WoTUE1LightAppendInteger(Out, Row.Brightness);
		WoTUE1LightAppendAscii(Out, " "); WoTUE1LightAppendInteger(Out, Row.Hue);
		WoTUE1LightAppendAscii(Out, " "); WoTUE1LightAppendInteger(Out, Row.Saturation);
		WoTUE1LightAppendAscii(Out, " "); WoTUE1LightAppendFloat(Out, Row.Radius);
		WoTUE1LightAppendAscii(Out, " ");
		WoTUE1LightAppendInteger(Out, static_cast<uint8>(Row.Shape));
		WoTUE1LightAppendAscii(Out, " ");
		WoTUE1LightAppendInteger(Out, static_cast<uint8>(Row.Type));
		WoTUE1LightAppendAscii(Out, " "); WoTUE1LightAppendInteger(Out, Row.Period);
		WoTUE1LightAppendAscii(Out, " "); WoTUE1LightAppendInteger(Out, Row.Phase);
		WoTUE1LightAppendAscii(Out, "\n");
	}
}

FString FWoTUE1LightRegistryBuilder::CanonicalDigest(
	const FWoTUE1LightSet& Set)
{
	TArray<uint8> Bytes;
	SerializeCanonical(Set, Bytes);
	return Sha256(Bytes).ToHex();
}

bool FWoTUE1LightRegistryBuilder::AreEquivalent(
	const FWoTUE1LightSet& Built, const FWoTUE1LightSet& Expected,
	FString& OutError)
{
	TSet<FName> NameSet;
	for (const TPair<FName, FWoTUE1LightRow>& Pair : Built.Rows) NameSet.Add(Pair.Key);
	for (const TPair<FName, FWoTUE1LightRow>& Pair : Expected.Rows) NameSet.Add(Pair.Key);
	TArray<FName> Names = NameSet.Array();
	Names.Sort([](FName Left, FName Right)
	{
		return Left.ToString().Compare(Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (const FName Name : Names)
	{
		const FWoTUE1LightRow* Left = Built.Rows.Find(Name);
		const FWoTUE1LightRow* Right = Expected.Rows.Find(Name);
		if (Left == nullptr || Right == nullptr)
		{
			OutError = FString::Printf(TEXT("UE1 light '%s' is missing from %s data"),
				*Name.ToString(), Left == nullptr ? TEXT("built") : TEXT("expected"));
			return false;
		}
		if (Left->Brightness != Right->Brightness) return WoTUE1LightFailCell(OutError, Name, TEXT("Brightness"));
		if (Left->Hue != Right->Hue) return WoTUE1LightFailCell(OutError, Name, TEXT("Hue"));
		if (Left->Saturation != Right->Saturation) return WoTUE1LightFailCell(OutError, Name, TEXT("Saturation"));
		if (WoTUE1LightFloatBits(Left->Radius) != WoTUE1LightFloatBits(Right->Radius)) return WoTUE1LightFailCell(OutError, Name, TEXT("Radius"));
		if (Left->Shape != Right->Shape) return WoTUE1LightFailCell(OutError, Name, TEXT("Shape"));
		if (Left->Type != Right->Type) return WoTUE1LightFailCell(OutError, Name, TEXT("Type"));
		if (Left->Period != Right->Period) return WoTUE1LightFailCell(OutError, Name, TEXT("Period"));
		if (Left->Phase != Right->Phase) return WoTUE1LightFailCell(OutError, Name, TEXT("Phase"));
	}
	OutError.Reset();
	return true;
}
