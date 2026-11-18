#include "WoTMeshFX.h"

#include "WoTRetailBuilderCanonical.h"
#include "Assets/WoTRetailIdentity.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTMeshClassSpec
{
	const TCHAR* Config;
	const TCHAR* ClassIdentity;
	const TCHAR* MeshIdentity;
};

constexpr FWoTMeshClassSpec GWoTMeshClasses[] = {
	{TEXT("SoulBarbSkull"), TEXT("Angreal.SoulBarbSkull"), TEXT("Angreal.SoulSkull")},
	{TEXT("SoulBarbImpact"), TEXT("WOT.SoulBarbImpact"), TEXT("WOT.SoulBarbImpact")},
	{TEXT("AbsorbVisual"), TEXT("Angreal.AbsorbVisual"), TEXT("Angreal.AbsorbVisual")},
	{TEXT("TaintExpDarts"), TEXT("Angreal.TaintExpDarts"), TEXT("Angreal.TaintExpDarts")},
	{TEXT("TaintExpTorus"), TEXT("Angreal.TaintExpTorus"), TEXT("Angreal.TaintExpTorus")},
	{TEXT("TaintExpGeoA"), TEXT("Angreal.TaintExpGeoA"), TEXT("Angreal.TaintExpGeoA")},
	{TEXT("TaintExpGeoB"), TEXT("Angreal.TaintExpGeoB"), TEXT("Angreal.TaintExpGeoB")},
	{TEXT("FrozenPresentation"), TEXT("Angreal.IceLeech"), TEXT("Angreal.Frozen")},
	{TEXT("AirShieldVisual"), TEXT("Angreal.AirShieldVisual"), TEXT("Angreal.ShieldParticleMesh")},
	{TEXT("EarthShieldVisual"), TEXT("Angreal.EarthShieldVisual"), TEXT("Angreal.ShieldParticleMesh")},
	{TEXT("FireShieldVisual"), TEXT("Angreal.FireShieldVisual"), TEXT("Angreal.ShieldParticleMesh")},
	{TEXT("SpiritShieldVisual"), TEXT("Angreal.SpiritShieldVisual"), TEXT("Angreal.ShieldParticleMesh")},
	{TEXT("WaterShieldVisual"), TEXT("Angreal.WaterShieldVisual"), TEXT("Angreal.ShieldParticleMesh")},
	{TEXT("LSImpact"), TEXT("Angreal.LSImpact"), TEXT("Angreal.LSImpact")},
	{TEXT("LSRing"), TEXT("Angreal.LSring"), TEXT("Angreal.LSring")},
	{TEXT("BFExpGlobe"), TEXT("Angreal.BFExpSprayer"), TEXT("Angreal.AMAPearl")},
	{TEXT("PixieJar"), TEXT("Angreal.PixieJar"), TEXT("Angreal.AMAPearl")},
	{TEXT("ArrowTrail"), TEXT("WoTPawns.ArrowTrail"), TEXT("WoTPawns.ArrowTrail")},
};

FName WoTMeshLeaf(FStringView Identity)
{
	const FString Text(Identity);
	int32 Dot = INDEX_NONE;
	return FName(*(Text.FindLastChar(TEXT('.'), Dot)
		? Text.RightChop(Dot + 1) : Text));
}

FName WoTMeshPackage(FStringView Identity)
{
	const FString Text(Identity);
	int32 Dot = INDEX_NONE;
	return FName(*(Text.FindChar(TEXT('.'), Dot) ? Text.Left(Dot) : Text));
}

bool WoTMeshTexture(const FWoTRetailBuildContext& Context,
	FStringView Identity, FWoTRetailTextureFacts* OutFacts, FString& Error)
{
	FWoTRetailTextureFacts Facts;
	if (!Context.FindTexture || !Context.FindTexture(Identity, Facts))
	{
		Error = FString::Printf(TEXT("mesh-fx texture '%s' is unavailable"),
			*FString(Identity)); return false;
	}
	if (OutFacts != nullptr) *OutFacts = Facts;
	return true;
}

bool WoTMeshReadOptionalFloat(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, float& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* P = Defaults.FindClassProperty(Identity, Name);
	if (P == nullptr) return true;
	if (!WoTRuntimeWorldPropertyFloat(*P, Out))
	{
		Error = FString::Printf(TEXT("mesh-fx '%s' property %s is malformed"),
			Identity, Name); return false;
	}
	return true;
}

bool WoTMeshReadOptionalBool(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, bool& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* P = Defaults.FindClassProperty(Identity, Name);
	if (P == nullptr) return true;
	if (!WoTRuntimeWorldPropertyBool(*P, Out))
	{
		Error = FString::Printf(TEXT("mesh-fx '%s' property %s is malformed"),
			Identity, Name); return false;
	}
	return true;
}

bool WoTMeshReadOptionalByte(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, float& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* P = Defaults.FindClassProperty(Identity, Name);
	if (P == nullptr) return true;
	uint8 Value = 0;
	if (!WoTRuntimeWorldPropertyByte(*P, Value))
	{
		Error = FString::Printf(TEXT("mesh-fx '%s' property %s is malformed"),
			Identity, Name); return false;
	}
	Out = static_cast<float>(Value);
	return true;
}

bool WoTMeshReadObject(const FWoTRetailBuildContext& Context,
	const TCHAR* Identity, const TCHAR* PropertyName, FString& Out,
	FString& Error, uint32 Index = 0)
{
	const FWoTRuntimeWorldProperty* P = Context.ClassDefaults.FindClassProperty(
		Identity, PropertyName, Index);
	if (P == nullptr || P->Type != EWoTRuntimeWorldPropertyType::Object
		|| P->ObjectIdentity.IsEmpty())
	{
		Error = FString::Printf(TEXT("mesh-fx '%s' property %s[%u] is missing or malformed"),
			Identity, PropertyName, Index); return false;
	}
	Out = P->ObjectIdentity;
	return true;
}

bool WoTMeshReadSkin(const FWoTRetailBuildContext& Context,
	FStringView TextureIdentity, FWoTMeshEffectRow& Row, FString& Error)
{
	FWoTRetailTextureFacts Facts;
	if (!WoTMeshTexture(Context, TextureIdentity, &Facts, Error)) return false;
	FWoTRetailObjectPath Path;
	if (!WoTParseRetailObjectPath(TextureIdentity, Path))
	{
		Error = FString::Printf(TEXT("mesh-fx texture identity '%s' is malformed"),
			*FString(TextureIdentity)); return false;
	}
	Row.SkinPackage = FName(*Path.Package);
	if (Facts.bProcedural) Row.ProceduralMaterialName = FName(*Path.Name);
	else
	{
		Row.SkinNames.Add(FName(*Path.Name));
		Row.SkinIdentities.Add(FString(TextureIdentity));
	}
	return true;
}

bool WoTMeshReadSkinProperty(const FWoTRetailBuildContext& Context,
	const TCHAR* Identity, const TCHAR* PropertyName, FWoTMeshEffectRow& Row,
	FString& Error)
{
	FString TextureIdentity;
	return WoTMeshReadObject(Context, Identity, PropertyName, TextureIdentity, Error)
		&& WoTMeshReadSkin(Context, TextureIdentity, Row, Error);
}

bool WoTMeshReadSkinSeries(const FWoTRetailBuildContext& Context,
	const TCHAR* Identity, const TCHAR* PropertyName, int32 Count,
	FWoTMeshEffectRow& Row, FString& Error)
{
	Row.SkinNames.Reset();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FString TextureIdentity;
		if (!WoTMeshReadObject(Context, Identity, PropertyName, TextureIdentity,
			Error, static_cast<uint32>(Index))
			|| !WoTMeshReadSkin(Context, TextureIdentity, Row, Error)) return false;
	}
	return true;
}

void WoTMeshApplyBehaviorPolicy(FName Config, FWoTMeshEffectRow& Row)
{
	// These flags are projections of the cited script methods. Asset identities
	// and default values are decoded separately from the retail source packages.
	if (Config == TEXT("SoulBarbSkull"))
	{
		// SoulBarbSkull.uc:48-79 randomizes motion and advances skins over life.
		Row.AnimName = TEXT("All");
	}
	else if (Config == TEXT("SoulBarbImpact"))
	{
		// SoulBarbImpact.uc:93-132 follows its owner and steps mesh/skin frames.
		Row.bPlayFrameSequence = true; Row.bFollow = true;
		Row.bHasLight = true; Row.LightRegistryKey = TEXT("SoulBarbImpact");
		Row.LifeSpan = 0.9f;
	}
	else if (Config == TEXT("AbsorbVisual"))
	{
		// AbsorbVisual.uc:36-43 plays All once while based to its owner.
		Row.AnimName = TEXT("All"); Row.bFollow = true;
	}
	else if (Config.ToString().StartsWith(TEXT("TaintExp")))
	{
		// TaintExpAssets.uc:17-53 plays All, follows, and fades over the
		// animation-derived lifetime.
		Row.AnimName = TEXT("All"); Row.bFollow = true; Row.bFadeEnvelope = true;
		if (Config == TEXT("TaintExpTorus"))
		{
			Row.bHasLight = true; Row.LightRegistryKey = TEXT("TaintExpTorus");
		}
	}
	else if (Config == TEXT("FrozenPresentation"))
	{
		// IceLeech.uc:62-185 follows its host, holds FROZEN, then tweens
		// MELTED and fades during MeltTime.
		Row.AnimName = TEXT("FROZEN"); Row.MeltAnim = TEXT("MELTED");
		Row.bFollow = true; Row.bHasLight = true;
	}
	else if (Config.ToString().EndsWith(TEXT("ShieldVisual")))
	{
		// ShieldParticleMesh.uc:52-126 loops All and its texture series while
		// following the owner through the authored fade envelope.
		Row.AnimName = TEXT("All"); Row.bLoopAnim = true;
		Row.bLoopSkinSeries = true; Row.bFollow = true;
		Row.bHasLight = true; Row.LightRegistryKey = TEXT("ShieldParticleMesh");
		Row.bOverrideLightColor = true;
		if (Config != TEXT("AirShieldVisual")) Row.LightSaturation = 0.0f;
	}
	else if (Config == TEXT("LSImpact"))
	{
		// LSImpact.uc:49-60 loops the authored skin series at FrameInterval.
		Row.bLoopSkinSeries = true; Row.bFollow = true;
		Row.bHasLight = true; Row.LightRegistryKey = TEXT("LSImpact");
		Row.LifeSpan = 0.0f;
	}
	else if (Config == TEXT("LSRing"))
	{
		// LSring.uc:25-30 chooses a random skin every tick.
		Row.bRandomSkinEachTick = true;
		Row.LifeSpan = 0.0f;
	}
	else if (Config == TEXT("BFExpGlobe"))
	{
		// BFExpSprayer.uc:25-41 creates a separate globe; sprayer lighting does
		// not become mesh ambient glow.
		Row.AmbientGlow = 0.0f;
	}
	else if (Config == TEXT("PixieJar"))
	{
		// PixieJar.uc:71-103 keeps the glass globe alive with its owner.
		Row.LifeSpan = 0.0f;
	}
	else if (Config == TEXT("ArrowTrail"))
	{
		// ArrowTrail.uc:18-21 shrinks DrawScale 10 to zero over LifeSpan .5.
		Row.MinGrowRate = Row.MaxGrowRate = -20.0f;
	}
}

bool WoTMeshDecodeCommon(const FWoTRetailBuildContext& Context,
	const FWoTMeshClassSpec& Spec, FWoTMeshEffectRow& Row, FString& Error)
{
	TArray<FString> Chain;
	if (!Context.ClassDefaults.GetClassChain(Spec.ClassIdentity, Chain))
	{
		Error = FString::Printf(TEXT("mesh-fx class '%s' is missing"),
			Spec.ClassIdentity); return false;
	}
	TArray<FWoTVertexMeshSequence> Sequences;
	if (!Context.FindMeshSequences
		|| !Context.FindMeshSequences(Spec.MeshIdentity, Sequences))
	{
		Error = FString::Printf(TEXT("mesh-fx mesh '%s' is unavailable"),
			Spec.MeshIdentity); return false;
	}
	if (!Row.AnimName.IsNone()
		&& !Sequences.ContainsByPredicate([&Row](const FWoTVertexMeshSequence& S)
		{
			return S.Name.Equals(Row.AnimName.ToString(), ESearchCase::IgnoreCase);
		}))
	{
		Error = FString::Printf(TEXT("mesh-fx mesh '%s' lacks sequence '%s'"),
			Spec.MeshIdentity, *Row.AnimName.ToString()); return false;
	}
	Row.MeshName = WoTMeshLeaf(Spec.MeshIdentity);
	if (!WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
		TEXT("LifeSpan"), Row.LifeSpan, Error)
		|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
			TEXT("DrawScale"), Row.DrawScale, Error)
		|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
			TEXT("ScaleGlow"), Row.ScaleGlow, Error)
		|| !WoTMeshReadOptionalByte(Context.ClassDefaults, Spec.ClassIdentity,
			TEXT("AmbientGlow"), Row.AmbientGlow, Error)
		|| !WoTMeshReadOptionalBool(Context.ClassDefaults, Spec.ClassIdentity,
			TEXT("bUnlit"), Row.bUnlit, Error)
		|| !WoTMeshReadOptionalBool(Context.ClassDefaults, Spec.ClassIdentity,
			TEXT("bMeshCurvy"), Row.bMeshCurvy, Error)
		|| !WoTMeshReadOptionalBool(Context.ClassDefaults, Spec.ClassIdentity,
			TEXT("bMeshEnviroMap"), Row.bEnvironmentMap, Error)) return false;
	if (const FWoTRuntimeWorldProperty* Style =
		Context.ClassDefaults.FindClassProperty(Spec.ClassIdentity, TEXT("Style")))
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*Style, Value))
		{
			Error = FString::Printf(TEXT("mesh-fx '%s' Style is malformed"),
				Spec.ClassIdentity); return false;
		}
		Row.Style = Value == 4 ? EWoTMeshEffectStyle::Modulated
			: Value == 3 ? EWoTMeshEffectStyle::Translucent
			: EWoTMeshEffectStyle::Normal;
	}
	return true;
}

bool WoTMeshDecodeIceExceptions(const FWoTRetailBuildContext& Context,
	FWoTMeshEffectRow& Row, FWoTMeshFXSet& Set, FString& Error)
{
	Row.ClassScaleOverrides.Reset();
	for (uint32 Index = 0; Index < 4; ++Index)
	{
		const FWoTRuntimeWorldProperty* P = Context.ClassDefaults.FindClassProperty(
			TEXT("Angreal.IceLeech"), TEXT("Exceptions"), Index);
		if (P == nullptr || P->Type != EWoTRuntimeWorldPropertyType::Struct)
		{
			Error = FString::Printf(TEXT("IceLeech Exceptions[%u] is missing or malformed"),
				Index); return false;
		}
		const FWoTRuntimeWorldProperty* ClassType =
			WoTRuntimeWorldStructMember(*P, TEXT("ClassType"));
		const FWoTRuntimeWorldProperty* Scale =
			WoTRuntimeWorldStructMember(*P, TEXT("Scale"));
		float ScaleValue = 0.0f;
		if (ClassType == nullptr || Scale == nullptr
			|| ClassType->Type != EWoTRuntimeWorldPropertyType::Name
			|| !WoTRuntimeWorldPropertyFloat(*Scale, ScaleValue))
		{
			Error = FString::Printf(TEXT("IceLeech Exceptions[%u] members are malformed"),
				Index); return false;
		}
		Row.ClassScaleOverrides.Emplace(FName(*ClassType->NameValue), ScaleValue);
	}
	for (const TCHAR* Leaf : {TEXT("Legion"), TEXT("BATrolloc"), TEXT("Trolloc"),
		TEXT("TrollocStrong"), TEXT("Minion")})
	{
		TArray<FString> Chain;
		const FString Identity = FString(TEXT("WoTPawns.")) + Leaf;
		if (!Context.ClassDefaults.GetClassChain(Identity, Chain))
		{
			Error = FString::Printf(TEXT("IceLeech exception class '%s' is missing"),
				*Identity); return false;
		}
		TArray<FName>& Names = Set.ClassChains.Add(FName(Leaf));
		for (const FString& Item : Chain) Names.Add(WoTMeshLeaf(Item));
	}
	return true;
}

struct FWoTSpriteSpec
{
	FString Config;
	FString ClassIdentity;
	FString TextureProperty;
	FString ScalarIdentity;
	int32 TrapIconIndex = INDEX_NONE;
};

TArray<FWoTSpriteSpec> WoTSpriteSpecs()
{
	TArray<FWoTSpriteSpec> Specs = {
		{TEXT("SoulBarbEffect"), TEXT("Angreal.SoulBarbEffect"), TEXT("Texture"),
			TEXT("Angreal.SoulBarbEffect")},
		{TEXT("SeekerGlobe"), TEXT("Angreal.AngrealSeekerGlobe"), TEXT("Texture"),
			TEXT("Angreal.AngrealSeekerGlobe")},
	};
	for (const TCHAR* Color : {TEXT("blue"), TEXT("red"), TEXT("green"),
		TEXT("yellow"), TEXT("purple")})
	{
		const FString Class = FString(Color).Left(1).ToUpper()
			+ FString(Color).RightChop(1) + TEXT("Dart");
		Specs.Add({FString(TEXT("DartCorona")) + Color,
			FString(TEXT("Angreal.")) + Class,
			TEXT("CoronaTexture"), TEXT("Angreal.FakeCorona")});
	}
	struct FTrap { const TCHAR* Type; };
	int32 TrapIndex = 0;
	for (const FTrap& Trap : {FTrap{TEXT("WallSlab")},
		FTrap{TEXT("Pit")},
		FTrap{TEXT("Portcullis")},
		FTrap{TEXT("Spear")},
		FTrap{TEXT("Staircase")},
		FTrap{TEXT("AngrealExpWardProjectile")}})
	{
		Specs.Add({FString(TEXT("TrapIcon")) + Trap.Type,
			TEXT("Angreal.TrapDetectLeech"),
			TEXT(""), TEXT("Angreal.DefaultTrapIcon"), TrapIndex++});
	}
	Specs.Add({TEXT("LightGlobeStar"), TEXT("Angreal.LightGlobeStar"),
		TEXT("Texture"), TEXT("Angreal.LightGlobeStar")});
	Specs.Add({TEXT("BalefireHand"), TEXT("Angreal.BalefireCorona"),
		TEXT("Texture"), TEXT("Angreal.BalefireCorona")});
	return Specs;
}

bool WoTMeshBuildTextureAnimation(const FWoTRetailBuildContext& Context,
	FStringView StartIdentity, FWoTTextureAnimationRow& Out, FString& Error)
{
	Out = {};
	FString Identity(StartIdentity);
	TSet<FString> Seen;
	for (int32 Index = 0; Index < 1024; ++Index)
	{
		if (Seen.Contains(Identity))
		{
			if (Identity == StartIdentity) return true;
			Error = FString::Printf(TEXT("texture animation from '%s' has a non-tail cycle"),
				*FString(StartIdentity));
			return false;
		}
		Seen.Add(Identity);
		FWoTRetailTextureFacts Facts;
		if (!WoTMeshTexture(Context, Identity, &Facts, Error)) return false;
		if (Out.FrameIdentities.IsEmpty())
		{
			Out.MinFrameRate = Facts.MinFrameRate;
			Out.MaxFrameRate = Facts.MaxFrameRate;
			Out.TexWidth = static_cast<float>(Facts.USize);
			Out.TexHeight = static_cast<float>(Facts.VSize);
		}
		Out.FrameIdentities.Add(Identity);
		if (Facts.AnimNext.IsEmpty()) return true;
		Identity = Facts.AnimNext;
	}
	Error = FString::Printf(TEXT("texture animation from '%s' exceeds 1024 frames"),
		*FString(StartIdentity));
	return false;
}

bool WoTMeshAddTextureAnimation(const FWoTRetailBuildContext& Context,
	FWoTMeshFXSet& Set, FStringView Identity, FString& Error)
{
	if (Set.TextureAnimations.Contains(FString(Identity))) return true;
	FWoTTextureAnimationRow Row;
	if (!WoTMeshBuildTextureAnimation(Context, Identity, Row, Error)) return false;
	Set.TextureAnimations.Add(FString(Identity), MoveTemp(Row));
	return true;
}

void WoTMeshSerializeNames(TArray<uint8>& Out, const TArray<FName>& Values)
{
	using namespace WoTRetailBuilderCanonical;
	Integer(Out, Values.Num());
	for (FName Value : Values) { Ascii(Out, " "); Name(Out, Value); }
}

void WoTMeshSerializeStrings(TArray<uint8>& Out, const TArray<FString>& Values)
{
	using namespace WoTRetailBuilderCanonical;
	Integer(Out, Values.Num());
	for (const FString& Value : Values) { Ascii(Out, " "); String(Out, Value); }
}
}

bool FWoTMeshFXBuilder::Build(const FWoTRetailBuildContext& Context,
	FWoTMeshFXSet& Out, FString& OutError)
{
	if (!Context.ClassDefaults.IsLoaded())
	{
		OutError = TEXT("mesh-fx requires loaded class defaults"); return false;
	}
	FWoTMeshFXSet Candidate;
	for (const FWoTMeshClassSpec& Spec : GWoTMeshClasses)
	{
		FWoTMeshEffectRow* Row = &Candidate.MeshEffects.Add(Spec.Config);
		WoTMeshApplyBehaviorPolicy(Spec.Config, *Row);
		if (FCString::Stricmp(Spec.Config, TEXT("BFExpGlobe")) == 0)
		{
			// BFExpSprayer's actor defaults describe the sprayer, while this row
			// describes the GenericSprite created in PreBeginPlay
			// (BFExpSprayer.uc:25-41). Validate the class/mesh, then project that
			// method's spawned-globe behavior.
			if (!WoTMeshDecodeCommon(Context, Spec, *Row, OutError)) return false;
			Row->MeshName = WoTMeshLeaf(Spec.MeshIdentity);
			if (!WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("LightDuration"), Row->LifeSpan, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("GlobeScaleMin"), Row->DrawScale, OutError)) return false;
			Row->bUnlit = true; Row->bMeshCurvy = true;
			Row->Style = EWoTMeshEffectStyle::Translucent;
			if (!WoTMeshReadSkin(Context, TEXT("Angreal.Effects.Glass"), *Row,
				OutError)) return false;
		}
		else if (!WoTMeshDecodeCommon(Context, Spec, *Row, OutError))
			return false;

		const FName Config(Spec.Config);
		if (Config == TEXT("SoulBarbSkull"))
		{
			if (!WoTMeshReadSkinSeries(Context, Spec.ClassIdentity, TEXT("AnimTextures"),
				7, *Row, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("MinSpinRate"), Row->MinSpinRate, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("MaxSpinRate"), Row->MaxSpinRate, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("MinRiseRate"), Row->MinRiseRate, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("MaxRiseRate"), Row->MaxRiseRate, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("MinGrowRate"), Row->MinGrowRate, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("MaxGrowRate"), Row->MaxGrowRate, OutError)) return false;
			Row->SkinStepSeconds = Row->LifeSpan / Row->SkinNames.Num();
		}
		else if (Config == TEXT("SoulBarbImpact"))
		{
			if (!WoTMeshReadSkinSeries(Context, Spec.ClassIdentity, TEXT("SkinAnim"),
				18, *Row, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("FrameInterval"), Row->SkinStepSeconds, OutError)) return false;
			Row->LifeSpan = Row->SkinStepSeconds * Row->SkinNames.Num();
		}
		else if (Config == TEXT("AbsorbVisual"))
		{
			if (!WoTMeshReadSkinProperty(Context, Spec.ClassIdentity, TEXT("Texture"),
				*Row, OutError)) return false;
			TArray<FWoTVertexMeshSequence> Sequences;
			if (!Context.FindMeshSequences
				|| !Context.FindMeshSequences(Spec.MeshIdentity, Sequences)) return false;
			const FWoTVertexMeshSequence* All = Sequences.FindByPredicate(
				[](const FWoTVertexMeshSequence& S)
				{ return S.Name.Equals(TEXT("All"), ESearchCase::IgnoreCase); });
			if (All == nullptr || All->Rate <= 0.0f)
			{
				OutError = TEXT("AbsorbVisual All sequence has no usable rate"); return false;
			}
			Row->LifeSpan = static_cast<float>(All->NumFrames) / All->Rate;
			// AbsorbVisual.uc sets bMeshCurvy, but the mesh is a ring of flat
			// fins, not a sphere; rounding only makes them wavy. Retail shipped
			// CurvedSurfaces=False, so it stays flat.
			Row->bMeshCurvy = false;
		}
		else if (Config.ToString().StartsWith(TEXT("TaintExp")))
		{
			if (!WoTMeshReadSkinProperty(Context, Spec.ClassIdentity, TEXT("Texture"),
				*Row, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("AnimationRate"), Row->AnimRate, OutError)) return false;
			const FWoTRuntimeWorldProperty* FrameCountProperty =
				Context.ClassDefaults.FindClassProperty(
					Spec.ClassIdentity, TEXT("NumAnimFrames"));
			int32 FrameCount = 0;
			if (FrameCountProperty == nullptr
				|| !WoTRuntimeWorldPropertyInteger(*FrameCountProperty, FrameCount))
			{
				OutError = FString::Printf(TEXT("mesh-fx '%s' NumAnimFrames is malformed"),
					Spec.ClassIdentity); return false;
			}
			// Round once from double: MSVC /fp:fast may rewrite the float form
			// (x / Rate / 30 -> x / Rate * (1/30)), which changed TaintExpTorus
			// by one ulp in the monolithic Win64 Test build and broke the digest.
			Row->LifeSpan = static_cast<float>(static_cast<double>(FrameCount)
				/ (static_cast<double>(Row->AnimRate) * 30.0));
		}
		else if (Config == TEXT("FrozenPresentation"))
		{
			if (!WoTMeshReadSkinProperty(Context, Spec.ClassIdentity, TEXT("Texture"),
				*Row, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("MeltTime"), Row->MeltTime, OutError)) return false;
			// AngrealIceProjectile.uc:97-142 supplies the leech duration.
			if (!WoTMeshReadOptionalFloat(Context.ClassDefaults,
				TEXT("Angreal.AngrealIceProjectile"), TEXT("Duration"), Row->LifeSpan,
				OutError)) return false;
			FString SoundIdentity;
			if (!WoTMeshReadObject(Context, Spec.ClassIdentity, TEXT("MeltingSound"),
				SoundIdentity, OutError)) return false;
			FWoTRetailObjectPath SoundPath;
			if (!WoTParseRetailObjectPath(SoundIdentity, SoundPath)) return false;
			Row->MeltSoundSubPath = FName(*WoTRetailAssetSubPath(
				EWoTNeutralResourceKind::Sound, SoundPath));
			for (const TCHAR* Field : {TEXT("LightBrightness"), TEXT("LightHue"),
				TEXT("LightSaturation"), TEXT("LightRadius")})
			{
				float* Target = FCString::Strcmp(Field, TEXT("LightBrightness")) == 0
					? &Row->LightBrightness
					: FCString::Strcmp(Field, TEXT("LightHue")) == 0 ? &Row->LightHue
					: FCString::Strcmp(Field, TEXT("LightSaturation")) == 0
						? &Row->LightSaturation : &Row->LightRadius;
				if (!WoTMeshReadOptionalByte(Context.ClassDefaults, Spec.ClassIdentity,
					Field, *Target, OutError)) return false;
			}
		}
		else if (Config.ToString().EndsWith(TEXT("ShieldVisual")))
		{
			if (!WoTMeshReadSkinSeries(Context, Spec.ClassIdentity, TEXT("TextureSet"),
				5, *Row, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("FadeInTime"), Row->FadeInTime, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("SustainTime"), Row->SustainTime, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("FadeOutTime"), Row->FadeOutTime, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("RotationYawRate"), Row->MinSpinRate, OutError)
				|| !WoTMeshReadOptionalByte(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("LightHue"), Row->LightHue, OutError)
				|| !WoTMeshReadOptionalByte(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("LightSaturation"), Row->LightSaturation, OutError)) return false;
			float SpriteRate = 0.0f;
			if (!WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
				TEXT("SpriteAnimationRate"), SpriteRate, OutError)) return false;
			Row->SkinStepSeconds = 1.0f / SpriteRate;
			Row->MaxSpinRate = Row->MinSpinRate;
		}
		else if (Config == TEXT("LSImpact"))
		{
			if (!WoTMeshReadSkinSeries(Context, Spec.ClassIdentity, TEXT("SkinAnim"),
				19, *Row, OutError)
				|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Spec.ClassIdentity,
					TEXT("FrameInterval"), Row->SkinStepSeconds, OutError)) return false;
		}
		else if (Config == TEXT("LSRing"))
		{
			if (!WoTMeshReadSkinSeries(Context, Spec.ClassIdentity, TEXT("Textures"),
				10, *Row, OutError)) return false;
		}
		else if (Config == TEXT("PixieJar"))
		{
			if (!WoTMeshReadSkinProperty(Context, Spec.ClassIdentity, TEXT("Skin"),
				*Row, OutError)) return false;
		}
		// ArrowTrail's JArrowTrail0 is a MESHMAP SETTEXTURE, already carried by
		// the imported mesh material; it is not an Actor.Skin property.
		// Common actor defaults are decoded first; method-driven lifetime and
		// palette behavior takes precedence where the cited scripts do so.
		WoTMeshApplyBehaviorPolicy(Config, *Row);
	}
	FWoTMeshEffectRow* Ice = Candidate.MeshEffects.Find(TEXT("FrozenPresentation"));
	if (Ice == nullptr || !WoTMeshDecodeIceExceptions(Context, *Ice, Candidate,
		OutError)) return false;

	// ReflectSkinEffect.uc:49-110 selects these palettes in SetColor. This is
	// script-method behavior; CDO scalar defaults are decoded below.
	auto AddReflect = [&Candidate](const TCHAR* Color, float Hue, float Saturation)
	{
		for (bool bMottled : {false, true})
		{
			FWoTPawnOverlayRow Row;
			Row.SkinNames.Add(FName(*FString::Printf(TEXT("Ref%s%s"),
				bMottled ? TEXT("M") : TEXT(""), Color)));
			Row.bFadeEnvelope = true; Row.bHasLight = true;
			Row.LightRegistryKey = TEXT("ReflectSkinEffect");
			Row.bOverrideLightColor = true;
			Row.LightHue = Hue; Row.LightSaturation = Saturation;
			Candidate.PawnOverlays.Add(FName(*FString::Printf(TEXT("Reflect%s%s"),
				Color, bMottled ? TEXT("Mottled") : TEXT(""))), MoveTemp(Row));
		}
	};
	AddReflect(TEXT("Green"), 64.0f, 0.0f);
	AddReflect(TEXT("Purple"), 212.0f, 0.0f);
	AddReflect(TEXT("Red"), 0.0f, 0.0f);
	AddReflect(TEXT("Blue"), 160.0f, 0.0f);
	AddReflect(TEXT("Yellow"), 32.0f, 0.0f);
	AddReflect(TEXT("White"), 0.0f, 255.0f);

	// LightningSkinEffect.uc:56-119 loops its skin series and spawns rings.
	FWoTPawnOverlayRow& LightningPolicy =
		Candidate.PawnOverlays.Add(TEXT("LightningSkinEffect"));
	LightningPolicy.bLoopSkinSeries = true;
	LightningPolicy.bStepSkinEveryTick = true;
	LightningPolicy.bHasLight = true;
	LightningPolicy.LightRegistryKey = TEXT("LightningSkinEffect");
	LightningPolicy.bSpawnLightningRings = true;

	for (TPair<FName, FWoTPawnOverlayRow>& Pair : Candidate.PawnOverlays)
	{
		const TCHAR* Identity = Pair.Key == TEXT("LightningSkinEffect")
			? TEXT("Angreal.LightningSkinEffect")
			: TEXT("Angreal.ReflectSkinEffect");
		FWoTPawnOverlayRow& Row = Pair.Value;
		if (!WoTMeshReadOptionalFloat(Context.ClassDefaults, Identity,
				TEXT("LifeSpan"), Row.LifeSpan, OutError)
			|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, Identity,
				TEXT("ScaleGlow"), Row.ScaleGlow, OutError)
			|| !WoTMeshReadOptionalByte(Context.ClassDefaults, Identity,
				TEXT("AmbientGlow"), Row.AmbientGlow, OutError)
			|| !WoTMeshReadOptionalByte(Context.ClassDefaults, Identity,
				TEXT("Fatness"), Row.Fatness, OutError)) return false;
	}
	if (FWoTPawnOverlayRow* Lightning =
		Candidate.PawnOverlays.Find(TEXT("LightningSkinEffect")))
	{
		Lightning->SkinNames.Reset();
		for (uint32 Index = 0; Index < 22; ++Index)
		{
			const FWoTRuntimeWorldProperty* Texture =
				Context.ClassDefaults.FindClassProperty(
					TEXT("Angreal.LightningSkinEffect"),
					TEXT("LightningTextures"), Index);
			if (Texture == nullptr
				|| Texture->Type != EWoTRuntimeWorldPropertyType::Object
				|| Texture->ObjectIdentity.IsEmpty())
			{
				OutError = FString::Printf(TEXT("LightningTextures[%u] is malformed"),
					Index); return false;
			}
			Lightning->SkinNames.Add(WoTMeshLeaf(Texture->ObjectIdentity));
			Lightning->SkinIdentities.Add(Texture->ObjectIdentity);
		}
	}
	for (const TCHAR* Color : {TEXT("Green"), TEXT("Purple"), TEXT("Red"),
		TEXT("Blue"), TEXT("Yellow"), TEXT("White")})
	{
		for (const TCHAR* Prefix : {TEXT("Ref"), TEXT("RefM")})
		{
			const FString Identity = Prefix[3] == TEXT('M')
				? FString::Printf(TEXT("Angreal.MottledEffects.%s%s"), Prefix, Color)
				: FString::Printf(TEXT("Angreal.Effects.%s%s"), Prefix, Color);
			if (!WoTMeshTexture(Context,
				Identity,
				nullptr, OutError)) return false;
		}
	}

	for (const FWoTSpriteSpec& Spec : WoTSpriteSpecs())
	{
		TArray<FString> Chain;
		if (!Context.ClassDefaults.GetClassChain(Spec.ClassIdentity, Chain))
		{
			OutError = FString::Printf(TEXT("sprite-fx class '%s' is missing"),
				*Spec.ClassIdentity); return false;
		}
		FString TextureIdentity;
		float TrapIconScale = 0.0f;
		if (Spec.TrapIconIndex != INDEX_NONE)
		{
			const FWoTRuntimeWorldProperty* Info =
				Context.ClassDefaults.FindClassProperty(Spec.ClassIdentity,
					TEXT("TrapIconInfos"), static_cast<uint32>(Spec.TrapIconIndex));
			const FWoTRuntimeWorldProperty* Texture = Info == nullptr ? nullptr
				: WoTRuntimeWorldStructMember(*Info, TEXT("TrapIcon"));
			const FWoTRuntimeWorldProperty* Scale = Info == nullptr ? nullptr
				: WoTRuntimeWorldStructMember(*Info, TEXT("IconScale"));
			if (Texture == nullptr || Scale == nullptr
				|| Texture->Type != EWoTRuntimeWorldPropertyType::Object
				|| Texture->ObjectIdentity.IsEmpty()
				|| !WoTRuntimeWorldPropertyFloat(*Scale, TrapIconScale))
			{
				OutError = FString::Printf(TEXT("sprite-fx TrapIconInfos[%d] is malformed"),
					Spec.TrapIconIndex); return false;
			}
			TextureIdentity = Texture->ObjectIdentity;
		}
		else
		{
			const FWoTRuntimeWorldProperty* Texture =
				Context.ClassDefaults.FindClassProperty(
					Spec.ClassIdentity, Spec.TextureProperty);
			if (Texture == nullptr
				|| Texture->Type != EWoTRuntimeWorldPropertyType::Object
				|| Texture->ObjectIdentity.IsEmpty())
			{
				OutError = FString::Printf(TEXT("sprite-fx class '%s' property %s is malformed"),
					*Spec.ClassIdentity, *Spec.TextureProperty); return false;
			}
			TextureIdentity = Texture->ObjectIdentity;
		}
		FWoTRetailTextureFacts Facts;
		if (!WoTMeshTexture(Context, TextureIdentity, &Facts, OutError)) return false;
		FWoTTextureAnimationRow Animation;
		if (!WoTMeshBuildTextureAnimation(Context, TextureIdentity, Animation,
			OutError)) return false;
		Candidate.TextureAnimations.Add(TextureIdentity, Animation);
		FWoTSpriteEffectRow* Row =
			&Candidate.SpriteEffects.Add(FName(*Spec.Config));
		if (Spec.Config == TEXT("SoulBarbEffect"))
		{
			// SoulBarbEffect.uc:9-20 advances one frame every AnimRate seconds.
			float FrameSeconds = 0.0f;
			if (!WoTMeshReadOptionalFloat(Context.ClassDefaults, *Spec.ClassIdentity,
				TEXT("AnimRate"), FrameSeconds, OutError) || FrameSeconds <= 0.0f)
				return false;
			Row->FrameRate = 1.0f / FrameSeconds;
		}
		Row->Package = WoTMeshPackage(TextureIdentity);
		Row->Frames.Reset();
		Row->FrameIdentities = Animation.FrameIdentities;
		for (const FString& Frame : Row->FrameIdentities)
			Row->Frames.Add(WoTMeshLeaf(Frame));
		Row->MinFrameRate = Animation.MinFrameRate;
		Row->MaxFrameRate = Animation.MaxFrameRate;
		Row->TexWidth = static_cast<float>(Facts.USize);
		Row->TexHeight = static_cast<float>(Facts.VSize);
		if (!WoTMeshReadOptionalFloat(Context.ClassDefaults, *Spec.ScalarIdentity,
			TEXT("DrawScale"), Row->DrawScale, OutError)
			|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, *Spec.ScalarIdentity,
				TEXT("ScaleGlow"), Row->ScaleGlow, OutError)
			|| !WoTMeshReadOptionalFloat(Context.ClassDefaults, *Spec.ScalarIdentity,
				TEXT("LifeSpan"), Row->LifeSpan, OutError)) return false;
		if (Spec.TrapIconIndex != INDEX_NONE) Row->DrawScale = TrapIconScale;
	}

	for (const TPair<FName, FWoTMeshEffectRow>& Pair : Candidate.MeshEffects)
	{
		for (const FString& Identity : Pair.Value.SkinIdentities)
		{
			if (!WoTMeshAddTextureAnimation(Context, Candidate, Identity, OutError))
				return false;
		}
	}
	for (const TPair<FName, FWoTPawnOverlayRow>& Pair : Candidate.PawnOverlays)
	{
		for (const FString& Identity : Pair.Value.SkinIdentities)
		{
			if (!WoTMeshAddTextureAnimation(Context, Candidate, Identity, OutError))
				return false;
		}
	}
	for (const TPair<const TCHAR*, const TCHAR*>& Spec : {
		TPair<const TCHAR*, const TCHAR*>(TEXT("AngrealEarthTremorProjectile"),
			TEXT("Angreal.AngrealEarthTremorProjectile")),
		TPair<const TCHAR*, const TCHAR*>(TEXT("AngrealDecayProjectile"),
			TEXT("Angreal.AngrealDecayProjectile")),
		TPair<const TCHAR*, const TCHAR*>(TEXT("AngrealSwapPlacesProjectile"),
			TEXT("Angreal.AngrealSwapPlacesProjectile"))})
	{
		FString Identity;
		if (!WoTMeshReadObject(Context, Spec.Value, TEXT("Texture"), Identity,
			OutError)) return false;
		Candidate.ClassTextureHeads.Add(FName(Spec.Key), Identity);
		if (!WoTMeshAddTextureAnimation(Context, Candidate, Identity, OutError))
			return false;
	}
	const FWoTRuntimeWorldProperty* NumStreakTextures =
		Context.ClassDefaults.FindClassProperty(TEXT("Angreal.LightningStreak02"),
			TEXT("NumTextures"));
	int32 StreakTextureCount = 0;
	if (NumStreakTextures == nullptr
		|| !WoTRuntimeWorldPropertyInteger(*NumStreakTextures, StreakTextureCount)
		|| StreakTextureCount <= 0 || StreakTextureCount > 256)
	{
		OutError = TEXT("LightningStreak02 NumTextures is malformed");
		return false;
	}
	for (int32 Index = 0; Index < StreakTextureCount; ++Index)
	{
		FString Identity;
		if (!WoTMeshReadObject(Context, TEXT("Angreal.LightningStreak02"),
			TEXT("Textures"), Identity, OutError, static_cast<uint32>(Index)))
			return false;
		Candidate.LightningStreakTextures.Add(Identity);
	}
	if (Candidate.MeshEffects.Num() != 18 || Candidate.PawnOverlays.Num() != 13
		|| Candidate.SpriteEffects.Num() != 15)
	{
		OutError = TEXT("mesh-fx census is not 18 mesh, 13 overlay, 15 sprite rows");
		return false;
	}
	Out = MoveTemp(Candidate); OutError.Reset(); return true;
}

void FWoTMeshFXBuilder::SerializeCanonical(const FWoTMeshFXSet& Set,
	TArray<uint8>& Out)
{
	using namespace WoTRetailBuilderCanonical;
	Out.Reset(); Ascii(Out, "wot-mesh-fx-v2\nmeshes ");
	Integer(Out, Set.MeshEffects.Num()); Ascii(Out, "\n");
	TArray<FName> Names; Set.MeshEffects.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		const FWoTMeshEffectRow& R = Set.MeshEffects.FindChecked(NameValue);
		Ascii(Out, "mesh "); Name(Out, NameValue);
		for (FName Value : {R.MeshName, R.SkinPackage, R.ProceduralMaterialName,
			R.AnimName, R.MeltAnim, R.MeltSoundSubPath, R.LightRegistryKey})
		{ Ascii(Out, " "); Name(Out, Value); }
		Ascii(Out, " skins "); WoTMeshSerializeNames(Out, R.SkinNames);
		Ascii(Out, " identities "); WoTMeshSerializeStrings(Out, R.SkinIdentities);
		for (float Value : {R.SkinStepSeconds, R.AnimRate, R.LifeSpan, R.DrawScale,
			R.ScaleGlow, R.AmbientGlow, R.FadeInTime, R.SustainTime, R.FadeOutTime,
			R.MeltTime, R.MinSpinRate, R.MaxSpinRate, R.MinRiseRate, R.MaxRiseRate,
			R.MinGrowRate, R.MaxGrowRate, R.LightBrightness, R.LightHue,
			R.LightSaturation, R.LightRadius})
		{ Ascii(Out, " "); Float(Out, Value); }
		for (bool Value : {R.bPlayFrameSequence, R.bLoopAnim, R.bLoopSkinSeries,
			R.bRandomSkinEachTick, R.bUnlit, R.bMeshCurvy, R.bFollow,
			R.bFadeEnvelope, R.bEnvironmentMap, R.bHasLight,
			R.bOverrideLightColor}) { Ascii(Out, " "); Bool(Out, Value); }
		Ascii(Out, " "); Integer(Out, static_cast<uint8>(R.Style));
		Ascii(Out, " scales "); Integer(Out, R.ClassScaleOverrides.Num());
		for (const TPair<FName, float>& Scale : R.ClassScaleOverrides)
		{ Ascii(Out, " "); Name(Out, Scale.Key); Ascii(Out, " "); Float(Out, Scale.Value); }
		Ascii(Out, "\n");
	}
	Ascii(Out, "overlays "); Integer(Out, Set.PawnOverlays.Num()); Ascii(Out, "\n");
	Names.Reset(); Set.PawnOverlays.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		const FWoTPawnOverlayRow& R = Set.PawnOverlays.FindChecked(NameValue);
		Ascii(Out, "overlay "); Name(Out, NameValue); Ascii(Out, " ");
		Name(Out, R.SkinPackage); Ascii(Out, " skins ");
		WoTMeshSerializeNames(Out, R.SkinNames);
		Ascii(Out, " identities "); WoTMeshSerializeStrings(Out, R.SkinIdentities);
		for (float Value : {R.SkinStepSeconds, R.LifeSpan, R.ScaleGlow,
			R.AmbientGlow, R.Fatness, R.LightBrightness, R.LightHue,
			R.LightSaturation, R.LightRadius})
		{ Ascii(Out, " "); Float(Out, Value); }
		for (bool Value : {R.bLoopSkinSeries, R.bStepSkinEveryTick,
			R.bFadeEnvelope, R.bHasLight,
			R.bOverrideLightColor, R.bSpawnLightningRings})
		{ Ascii(Out, " "); Bool(Out, Value); }
		Ascii(Out, " "); Name(Out, R.LightRegistryKey); Ascii(Out, "\n");
	}
	Ascii(Out, "sprites "); Integer(Out, Set.SpriteEffects.Num()); Ascii(Out, "\n");
	Names.Reset(); Set.SpriteEffects.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		const FWoTSpriteEffectRow& R = Set.SpriteEffects.FindChecked(NameValue);
		Ascii(Out, "sprite "); Name(Out, NameValue); Ascii(Out, " ");
		Name(Out, R.Package); Ascii(Out, " frames "); WoTMeshSerializeNames(Out, R.Frames);
		Ascii(Out, " identities "); WoTMeshSerializeStrings(Out, R.FrameIdentities);
		for (float Value : {R.TexWidth, R.TexHeight, R.DrawScale, R.ScaleGlow,
			R.FrameRate, R.MinFrameRate, R.MaxFrameRate, R.LifeSpan})
		{ Ascii(Out, " "); Float(Out, Value); }
		Ascii(Out, " "); Bool(Out, R.bModulated); Ascii(Out, "\n");
	}
	Ascii(Out, "chains "); Integer(Out, Set.ClassChains.Num()); Ascii(Out, "\n");
	Names.Reset(); Set.ClassChains.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		Ascii(Out, "chain "); Name(Out, NameValue); Ascii(Out, " ");
		WoTMeshSerializeNames(Out, Set.ClassChains.FindChecked(NameValue));
		Ascii(Out, "\n");
	}
	TArray<FString> Identities;
	Set.TextureAnimations.GetKeys(Identities);
	Identities.Sort([](const FString& A, const FString& B)
	{
		return Fold(A).Compare(Fold(B), ESearchCase::CaseSensitive) < 0;
	});
	Ascii(Out, "animations "); Integer(Out, Identities.Num()); Ascii(Out, "\n");
	for (const FString& Identity : Identities)
	{
		const FWoTTextureAnimationRow& R = Set.TextureAnimations.FindChecked(Identity);
		Ascii(Out, "animation "); String(Out, Identity); Ascii(Out, " frames ");
		WoTMeshSerializeStrings(Out, R.FrameIdentities);
		for (float Value : {R.MinFrameRate, R.MaxFrameRate, R.TexWidth, R.TexHeight})
		{ Ascii(Out, " "); Float(Out, Value); }
		Ascii(Out, "\n");
	}
	Ascii(Out, "class-textures "); Integer(Out, Set.ClassTextureHeads.Num());
	Ascii(Out, "\n");
	Names.Reset(); Set.ClassTextureHeads.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		Ascii(Out, "class-texture "); Name(Out, NameValue); Ascii(Out, " ");
		String(Out, Set.ClassTextureHeads.FindChecked(NameValue)); Ascii(Out, "\n");
	}
	Ascii(Out, "lightning-streak ");
	WoTMeshSerializeStrings(Out, Set.LightningStreakTextures); Ascii(Out, "\n");
}

FString FWoTMeshFXBuilder::CanonicalDigest(const FWoTMeshFXSet& Set)
{
	TArray<uint8> Bytes; SerializeCanonical(Set, Bytes); return Sha256(Bytes).ToHex();
}

bool FWoTMeshFXBuilder::AreEquivalent(const FWoTMeshFXSet& Built,
	const FWoTMeshFXSet& Expected, FString& OutError)
{
	TArray<uint8> A; TArray<uint8> B;
	SerializeCanonical(Built, A); SerializeCanonical(Expected, B);
	if (A != B)
	{
		int32 FirstByte = 0;
		while (FirstByte < A.Num() && FirstByte < B.Num()
			&& A[FirstByte] == B[FirstByte]) ++FirstByte;
		A.Add(0); B.Add(0);
		TArray<FString> ALines; TArray<FString> BLines;
		FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(A.GetData())))
			.ParseIntoArrayLines(ALines, false);
		FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(B.GetData())))
			.ParseIntoArrayLines(BLines, false);
		const int32 Common = FMath::Min(ALines.Num(), BLines.Num());
		int32 DifferenceCount = 0;
		for (int32 Index = 0; Index < Common; ++Index)
		{
			if (ALines[Index] != BLines[Index])
			{
				if (DifferenceCount++ < 12)
				{
					OutError += FString::Printf(TEXT("mesh-fx canonical line %d differs: built '%s', expected '%s'\n"),
						Index + 1, *ALines[Index], *BLines[Index]);
				}
			}
		}
		if (DifferenceCount > 0) return false;
		OutError = FString::Printf(TEXT("mesh-fx canonical bytes differ at %d (built size %d/value %u, expected size %d/value %u)"),
			FirstByte, A.Num() - 1,
			FirstByte < A.Num() - 1 ? A[FirstByte] : 0,
			B.Num() - 1, FirstByte < B.Num() - 1 ? B[FirstByte] : 0);
		return false;
	}
	OutError.Reset(); return true;
}
