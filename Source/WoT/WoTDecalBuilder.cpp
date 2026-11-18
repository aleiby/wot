#include "WoTDecal.h"

#include "WoTRetailBuilderCanonical.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTDecalClassSpec
{
	const TCHAR* Name;
	const TCHAR* Identity;
	const TCHAR* TextureProperty;
	int32 TextureCount;
};

// CDO data anchors: ParticleSystems/{BloodDecal,DecaySplatter,CrackDecal,
// ScorchMark,BurnDecal}.uc, Angreal/{BalefireDecal,BFHole}.uc, and
// WoTPawns/MashadarCrack.uc. Companion spawning, non-registration and burn-in
// lifecycle are method behavior and remain the compact native policy below.
constexpr FWoTDecalClassSpec GWoTDecals[] = {
	{TEXT("BloodDecalRed"), TEXT("ParticleSystems.BloodDecalRed"),
		TEXT("BloodTextures"), 15},
	{TEXT("BloodDecalBlack"), TEXT("ParticleSystems.BloodDecalBlack"),
		TEXT("BloodTextures"), 15},
	{TEXT("DecaySplatter"), TEXT("ParticleSystems.DecaySplatter"),
		TEXT("BloodTextures"), 15},
	{TEXT("BalefireDecal"), TEXT("Angreal.BalefireDecal"),
		TEXT("AnimTextures"), 10},
	{TEXT("BFHole"), TEXT("Angreal.BFHole"), TEXT("Skin"), 1},
	{TEXT("MashadarCrack"), TEXT("WoTPawns.MashadarCrack"),
		TEXT("BloodTextures"), 15},
	{TEXT("CrackDecal"), TEXT("ParticleSystems.CrackDecal"),
		TEXT("BloodTextures"), 15},
	{TEXT("ScorchMark"), TEXT("ParticleSystems.ScorchMark"),
		TEXT("BloodTextures"), 15},
	{TEXT("BurnDecal"), TEXT("ParticleSystems.BurnDecal"),
		TEXT("BloodTextures"), 15},
};

FName WoTDecalLeaf(FStringView Identity)
{
	const FString Text(Identity);
	int32 Dot = INDEX_NONE;
	return FName(*(Text.FindLastChar(TEXT('.'), Dot)
		? Text.RightChop(Dot + 1) : Text));
}

FName WoTDecalPackage(FStringView Identity)
{
	const FString Text(Identity);
	int32 Dot = INDEX_NONE;
	return FName(*(Text.FindChar(TEXT('.'), Dot) ? Text.Left(Dot) : Text));
}

bool WoTDecalReadObject(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* PropertyName, uint32 Index,
	FString& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property =
		Defaults.FindClassProperty(Identity, PropertyName, Index);
	if (Property == nullptr
		|| Property->Type != EWoTRuntimeWorldPropertyType::Object
		|| Property->ObjectIdentity.IsEmpty())
	{
		Error = FString::Printf(TEXT("decal '%s' property %s[%u] is missing or malformed"),
			Identity, PropertyName, Index);
		return false;
	}
	Out = Property->ObjectIdentity;
	return true;
}

bool WoTDecalReadFloat(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* PropertyName, float& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property =
		Defaults.FindClassProperty(Identity, PropertyName);
	if (Property == nullptr || !WoTRuntimeWorldPropertyFloat(*Property, Out))
	{
		Error = FString::Printf(TEXT("decal '%s' property %s is missing or malformed"),
			Identity, PropertyName);
		return false;
	}
	return true;
}

bool WoTDecalDecode(const FWoTRuntimeClassDefaults& Defaults,
	const FWoTDecalClassSpec& Spec, FWoTDecalConfig& Out, FString& Error)
{
	TArray<FString> Chain;
	if (!Defaults.GetClassChain(Spec.Identity, Chain))
	{
		Error = FString::Printf(TEXT("decal class '%s' is missing"), Spec.Identity);
		return false;
	}
	FString FirstTexture;
	for (int32 Index = 0; Index < Spec.TextureCount; ++Index)
	{
		FString Texture;
		if (!WoTDecalReadObject(Defaults, Spec.Identity, Spec.TextureProperty,
			static_cast<uint32>(Index), Texture, Error)) return false;
		if (Index == 0) FirstTexture = Texture;
		Out.Textures.Add(WoTDecalLeaf(Texture));
	}
	Out.Package = WoTDecalPackage(FirstTexture);

	if (FCString::Stricmp(Spec.Name, TEXT("DecaySplatter")) == 0)
	{
		if (!WoTDecalReadFloat(Defaults, Spec.Identity, TEXT("DecalDrawScale"),
			Out.DrawScale, Error)) return false;
		for (uint32 Index = 0; Index < 2; ++Index)
		{
			FString Texture;
			if (!WoTDecalReadObject(Defaults, Spec.Identity, TEXT("DropTextures"),
				Index, Texture, Error)) return false;
			Out.FlyingTextures.Add(WoTDecalLeaf(Texture));
		}
	}
	else if (!WoTDecalReadFloat(Defaults, Spec.Identity, TEXT("DrawScale"),
		Out.DrawScale, Error)) return false;

	if (const FWoTRuntimeWorldProperty* Style =
		Defaults.FindClassProperty(Spec.Identity, TEXT("Style")))
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*Style, Value))
		{
			Error = FString::Printf(TEXT("decal '%s' Style is malformed"), Spec.Identity);
			return false;
		}
		Out.Style = Value == 3
			? EWoTDecalStyle::Translucent : EWoTDecalStyle::Modulated;
	}
	if (const FWoTRuntimeWorldProperty* Ambient =
		Defaults.FindClassProperty(Spec.Identity, TEXT("AmbientGlow")))
	{
		if (!WoTRuntimeWorldPropertyByte(*Ambient, Out.AmbientGlow))
		{
			Error = FString::Printf(TEXT("decal '%s' AmbientGlow is malformed"),
				Spec.Identity); return false;
		}
	}
	for (const TPair<const TCHAR*, uint8*> Field : {
		TPair<const TCHAR*, uint8*>(TEXT("DetailLevel"), &Out.DetailLevel),
		TPair<const TCHAR*, uint8*>(TEXT("GoreLevel"), &Out.GoreLevel)})
	{
		if (const FWoTRuntimeWorldProperty* Property =
			Defaults.FindClassProperty(Spec.Identity, Field.Key);
			Property && !WoTRuntimeWorldPropertyByte(*Property, *Field.Value))
		{
			Error = FString::Printf(TEXT("decal '%s' %s is malformed"),
				Spec.Identity, Field.Key); return false;
		}
	}
	if (!WoTDecalReadFloat(Defaults, Spec.Identity, TEXT("VisibilityRadius"),
		Out.VisibilityRadius, Error)
		|| !WoTDecalReadFloat(Defaults, Spec.Identity, TEXT("VisibilityHeight"),
			Out.VisibilityHeight, Error)) return false;
	if (FCString::Stricmp(Spec.Name, TEXT("BalefireDecal")) == 0)
	{
		if (!WoTDecalReadFloat(Defaults, Spec.Identity, TEXT("FrameTime"),
			Out.FrameTime, Error)
			|| !WoTDecalReadFloat(Defaults, Spec.Identity, TEXT("FadeTime"),
				Out.FadeTime, Error)) return false;
		// BalefireDecal.Tick/AttemptRegistration, exports/script/Angreal/
		// BalefireDecal.uc:55-94.
		Out.bRegisters = false;
		Out.CompanionDecal = TEXT("BFHole");
	}
	else if (FCString::Stricmp(Spec.Name, TEXT("MashadarCrack")) == 0)
	{
		// MashadarCrack.AttemptRegistration is intentionally empty.
		Out.bRegisters = false;
	}
	return true;
}

bool WoTDecalConfigEqual(const FWoTDecalConfig& A, const FWoTDecalConfig& B)
{
	using WoTRetailBuilderCanonical::FloatBits;
	return A.Textures == B.Textures && A.FlyingTextures == B.FlyingTextures
		&& FloatBits(A.DrawScale) == FloatBits(B.DrawScale)
		&& A.Package == B.Package && A.Style == B.Style
		&& A.AmbientGlow == B.AmbientGlow
		&& A.DetailLevel == B.DetailLevel && A.GoreLevel == B.GoreLevel
		&& FloatBits(A.VisibilityRadius) == FloatBits(B.VisibilityRadius)
		&& FloatBits(A.VisibilityHeight) == FloatBits(B.VisibilityHeight)
		&& FloatBits(A.FrameTime) == FloatBits(B.FrameTime)
		&& FloatBits(A.FadeTime) == FloatBits(B.FadeTime)
		&& A.bRegisters == B.bRegisters
		&& A.CompanionDecal == B.CompanionDecal;
}
}

bool FWoTDecalBuilder::Build(const FWoTRetailBuildContext& Context,
	FWoTDecalSet& Out, FString& OutError)
{
	if (!Context.ClassDefaults.IsLoaded())
	{
		OutError = TEXT("decal requires loaded class defaults");
		return false;
	}
	FWoTDecalSet Candidate;
	for (const FWoTDecalClassSpec& Spec : GWoTDecals)
	{
		FWoTDecalConfig Config;
		if (!WoTDecalDecode(Context.ClassDefaults, Spec, Config, OutError))
			return false;
		Candidate.Configs.Add(FName(Spec.Name), MoveTemp(Config));
	}
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}

void FWoTDecalBuilder::SerializeCanonical(const FWoTDecalSet& Set,
	TArray<uint8>& Out)
{
	using namespace WoTRetailBuilderCanonical;
	Out.Reset();
	Ascii(Out, "wot-decal-v2\nconfigs "); Integer(Out, Set.Configs.Num());
	Ascii(Out, "\n");
	TArray<FName> Names; Set.Configs.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		const FWoTDecalConfig& C = Set.Configs.FindChecked(NameValue);
		Ascii(Out, "config "); Name(Out, NameValue); Ascii(Out, " ");
		Name(Out, C.Package); Ascii(Out, " "); Float(Out, C.DrawScale);
		Ascii(Out, " "); Integer(Out, static_cast<uint8>(C.Style));
		Ascii(Out, " "); Integer(Out, C.AmbientGlow);
		Ascii(Out, " "); Integer(Out, C.DetailLevel);
		Ascii(Out, " "); Integer(Out, C.GoreLevel);
		Ascii(Out, " "); Float(Out, C.VisibilityRadius);
		Ascii(Out, " "); Float(Out, C.VisibilityHeight);
		Ascii(Out, " "); Float(Out, C.FrameTime); Ascii(Out, " ");
		Float(Out, C.FadeTime); Ascii(Out, " "); Bool(Out, C.bRegisters);
		Ascii(Out, " "); Name(Out, C.CompanionDecal);
		Ascii(Out, " textures "); Integer(Out, C.Textures.Num());
		for (FName Texture : C.Textures) { Ascii(Out, " "); Name(Out, Texture); }
		Ascii(Out, " flying "); Integer(Out, C.FlyingTextures.Num());
		for (FName Texture : C.FlyingTextures) { Ascii(Out, " "); Name(Out, Texture); }
		Ascii(Out, "\n");
	}
}

FString FWoTDecalBuilder::CanonicalDigest(const FWoTDecalSet& Set)
{
	TArray<uint8> Bytes; SerializeCanonical(Set, Bytes); return Sha256(Bytes).ToHex();
}

bool FWoTDecalBuilder::AreEquivalent(const FWoTDecalSet& Built,
	const FWoTDecalSet& Expected, FString& OutError)
{
	if (Built.Configs.Num() != Expected.Configs.Num())
	{
		OutError = TEXT("decal config count differs"); return false;
	}
	for (const TPair<FName, FWoTDecalConfig>& Pair : Expected.Configs)
	{
		const FWoTDecalConfig* Actual = Built.Configs.Find(Pair.Key);
		if (Actual == nullptr || !WoTDecalConfigEqual(*Actual, Pair.Value))
		{
			OutError = Pair.Key.ToString() + TEXT(" decal config differs");
			return false;
		}
	}
	OutError.Reset(); return true;
}
