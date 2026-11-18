#include "WoTWeaveFX.h"

#include "WoTRetailBuilderCanonical.h"
#include "Assets/WoTRetailIdentity.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeParticleProjection.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTWeaveBundleSpec
{
	FName Name;
	FString Identity;
	TArray<FString> TextureOverrides;
	int32 TrapInfoIndex = INDEX_NONE;
};

FString WoTWeaveObjectSlug(FStringView Identity, bool bProcedural)
{
	FWoTRetailObjectPath Path;
	if (!WoTParseRetailObjectPath(Identity, Path)) return FString(Identity);
	return (bProcedural ? TEXT("FB:") : TEXT(""))
		+ WoTRetailPlainLogicalBase(Path);
}

FName WoTWeaveLeaf(FStringView Identity)
{
	FWoTRetailObjectPath Path;
	return WoTParseRetailObjectPath(Identity, Path)
		? FName(*Path.Name) : FName(Identity);
}

void WoTWeaveAddDirect(TArray<FWoTWeaveBundleSpec>& Specs,
	const TCHAR* Package, const TCHAR* Name)
{
	Specs.Add({FName(Name), FString::Printf(TEXT("%s.%s"), Package, Name), {}});
}

TArray<FWoTWeaveBundleSpec> WoTWeaveSpecs()
{
	TArray<FWoTWeaveBundleSpec> Specs;
	for (const TCHAR* Name : {TEXT("FireballSprayer2"), TEXT("AirBlastProj"),
		TEXT("AirBlastBall"), TEXT("FireballFlame"), TEXT("FireballSmoke"),
		TEXT("TracerSprayer"), TEXT("SnowSprayer01"), TEXT("SnowSprayer02"),
		TEXT("SnowSprayer03"), TEXT("SeekerSmoke"), TEXT("LightningSprayer02")})
		WoTWeaveAddDirect(Specs, TEXT("Angreal"), Name);
	WoTWeaveAddDirect(Specs, TEXT("WoTPawns"), TEXT("MashadarPainSprayer"));
	WoTWeaveAddDirect(Specs, TEXT("WoTPawns"), TEXT("MyrddraalTeleportSprayer2"));
	for (const TCHAR* Name : {TEXT("LegionStompSprayerA"),
		TEXT("LegionStompSprayerB"), TEXT("LegionStompSprayerC"),
		TEXT("LegionStompSprayerD")})
		WoTWeaveAddDirect(Specs, TEXT("WoTPawns"), Name);
	WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), TEXT("WaterSplash"));
	WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), TEXT("Flame01"));
	WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), TEXT("BlackSmoke01"));
	WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), TEXT("MyrdSprayerA"));
	WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), TEXT("MyrdSprayerB"));
	WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), TEXT("MyrdSprayerExp"));
	WoTWeaveAddDirect(Specs, TEXT("Angreal"), TEXT("SoulBarbSmoke"));
	WoTWeaveAddDirect(Specs, TEXT("Angreal"), TEXT("BlastBits"));

	auto AddAppear = [&Specs](const TCHAR* Name, const TCHAR* Texture)
	{
		Specs.Add({FName(Name), TEXT("ParticleSystems.AppearSprayer"),
			{FString::Printf(TEXT("ParticleSystems.Appear.%s"), Texture)}});
	};
	AddAppear(TEXT("AppearSprayerWhite"), TEXT("AWhiteCorona"));
	AddAppear(TEXT("AppearSprayerCyanCorona"), TEXT("CyanCorona"));
	AddAppear(TEXT("AppearSprayerPurpleCorona"), TEXT("PurpleCorona"));
	AddAppear(TEXT("AppearSprayerAOrangeCorona"), TEXT("AOrangeCorona"));
	AddAppear(TEXT("AppearSprayerABlueCorona"), TEXT("ABlueCorona"));
	AddAppear(TEXT("AppearSprayerAPinkCorona"), TEXT("APinkCorona"));
	AddAppear(TEXT("AppearSprayerAPurpleCorona"), TEXT("APurpleCorona"));
	AddAppear(TEXT("AppearSprayerRedCorona"), TEXT("RedCorona"));
	AddAppear(TEXT("AppearSprayerGreenCorona"), TEXT("GreenCorona"));
	AddAppear(TEXT("AppearSprayerYellowCorona"), TEXT("YellowCorona"));
	AddAppear(TEXT("AppearSprayerZGreenCorona"), TEXT("ZGreenCorona"));
	AddAppear(TEXT("AppearSprayerAYellowCorona"), TEXT("AYellowCorona"));
	AddAppear(TEXT("AppearSprayerAWhiteCorona"), TEXT("AWhiteCorona"));

	WoTWeaveAddDirect(Specs, TEXT("Angreal"), TEXT("WhirlwindDust"));
	WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), TEXT("LavaSpew"));
	struct FTrap { const TCHAR* Name; const TCHAR* Icon; const TCHAR* Sparkle; };
	int32 TrapInfoIndex = 0;
	for (const FTrap& Trap : {
		FTrap{TEXT("WallSlab"), TEXT("TrapWallParts"), TEXT("ParticleSystems.Appear.PurpleCorona")},
		FTrap{TEXT("Pit"), TEXT("TrapPitParts"), TEXT("ParticleSystems.Appear.AOrangeCorona")},
		FTrap{TEXT("Portcullis"), TEXT("TrapPortculParts"), TEXT("ParticleSystems.Appear.APinkCorona")},
		FTrap{TEXT("Spear"), TEXT("TrapSpearParts"), TEXT("ParticleSystems.Fire.Flame06")},
		FTrap{TEXT("Staircase"), TEXT("TrapStairParts"), TEXT("ParticleSystems.Appear.AYellowCorona")},
		FTrap{TEXT("AngrealExpWardProjectile"), TEXT("TrapExpWardPartsX"), TEXT("ParticleSystems.Appear.AYellowCorona")}})
	{
		Specs.Add({FName(*(FString(TEXT("TrapDetect")) + Trap.Name)),
			TEXT("Angreal.DefaultTrapSprayer"),
			{FString::Printf(TEXT("Angreal.TrapDetect.%s"), Trap.Icon), Trap.Sparkle},
			TrapInfoIndex++});
	}
	WoTWeaveAddDirect(Specs, TEXT("WOT"), TEXT("WOTSparks"));
	for (int32 Index = 1; Index <= 9; ++Index)
	{
		const FString Name = FString::Printf(TEXT("Firework%02d"), Index);
		WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), *Name);
		const FString NoLight = Name + TEXT("NL");
		WoTWeaveAddDirect(Specs, TEXT("ParticleSystems"), *NoLight);
	}
	WoTWeaveAddDirect(Specs, TEXT("Angreal"), TEXT("LightningSparks"));
	WoTWeaveAddDirect(Specs, TEXT("Angreal"), TEXT("BFSprayer"));
	struct FBFTexture { const TCHAR* Name; const TCHAR* Identity; };
	for (const FBFTexture& Texture : {
		FBFTexture{TEXT("PurpleCorona"), TEXT("ParticleSystems.Appear.PurpleCorona")},
		FBFTexture{TEXT("PurpleSpark"), TEXT("ParticleSystems.Appear.PurpleSpark")},
		FBFTexture{TEXT("WhiteCorona"), TEXT("ParticleSystems.Appear.WhiteCorona")},
		FBFTexture{TEXT("APurpleCorona"), TEXT("ParticleSystems.Appear.APurpleCorona")},
		FBFTexture{TEXT("BlueSpark"), TEXT("ParticleSystems.Appear.BlueSpark")},
		FBFTexture{TEXT("PF04"), TEXT("ParticleSystems.Flares.PF04")}})
	{
		Specs.Add({FName(*(FString(TEXT("BFSprayer")) + Texture.Name)),
			TEXT("Angreal.BFSprayer"), {Texture.Identity}});
	}
	WoTWeaveAddDirect(Specs, TEXT("Angreal"), TEXT("BFExpSprayer"));
	for (const TCHAR* Color : {TEXT("Blue"), TEXT("Green"), TEXT("Purple"),
		TEXT("Red"), TEXT("Yellow")})
	{
		const FString Name = FString(TEXT("PixieSprayer")) + Color;
		WoTWeaveAddDirect(Specs, TEXT("Angreal"), *Name);
	}
	for (const TCHAR* Color : {TEXT("Blue"), TEXT("Green"), TEXT("Purple"),
		TEXT("Red"), TEXT("Yellow")})
	{
		Specs.Add({FName(*(FString(TEXT("CarrySealSprayer")) + Color)),
			TEXT("WOT.CarrySealSprayer"),
			{FString::Printf(TEXT("WOT.CarryingSeal.CSseal%s"), Color)}});
	}
	return Specs;
}

bool WoTWeaveReadSprayer(const FWoTRuntimeClassDefaults& Defaults,
	const FWoTWeaveBundleSpec& Spec,
	FWoTRuntimeParticleSprayerConfiguration& Out, FString& Error)
{
	TArray<FString> Chain;
	if (!Defaults.GetClassChain(Spec.Identity, Chain))
	{
		Error = FString::Printf(TEXT("weave sprayer class '%s' is missing"),
			*Spec.Identity); return false;
	}
	uint8 Count = 0;
	const FWoTRuntimeWorldProperty* CountProperty =
		Defaults.FindClassProperty(Spec.Identity, TEXT("NumTemplates"));
	if (CountProperty == nullptr
		|| !WoTRuntimeWorldPropertyByte(*CountProperty, Count)
		|| Count == 0 || Count > 16
		|| (!Spec.TextureOverrides.IsEmpty()
			&& Count != Spec.TextureOverrides.Num()))
	{
		Error = FString::Printf(TEXT("weave sprayer '%s' template count is malformed"),
			*Spec.Identity); return false;
	}
	Out.Templates.SetNum(Count);
	Out.TextureIdentities.SetNum(Count);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const FWoTRuntimeWorldProperty* Template =
			Defaults.FindClassProperty(Spec.Identity, TEXT("Templates"), Index);
		if (Template == nullptr
			|| !WoTRuntimeReadParticleTemplate(*Template, Out.Templates[Index]))
		{
			Error = FString::Printf(TEXT("weave sprayer '%s' template %u is malformed"),
				*Spec.Identity, Index); return false;
		}
		if (!Spec.TextureOverrides.IsEmpty())
		{
			Out.TextureIdentities[Index] = Spec.TextureOverrides[Index];
		}
		else
		{
			const FWoTRuntimeWorldProperty* Texture =
				Defaults.FindClassProperty(Spec.Identity, TEXT("Particles"), Index);
			if (Texture == nullptr
				|| Texture->Type != EWoTRuntimeWorldPropertyType::Object
				|| Texture->ObjectIdentity.IsEmpty())
			{
				Error = FString::Printf(
					TEXT("weave sprayer '%s' particle %u is missing or malformed"),
					*Spec.Identity, Index); return false;
			}
			Out.TextureIdentities[Index] = Texture->ObjectIdentity;
		}
	}
	auto Float = [&](const TCHAR* Name, float& Value)
	{
		const FWoTRuntimeWorldProperty* P = Defaults.FindClassProperty(Spec.Identity, Name);
		return P == nullptr || WoTRuntimeWorldPropertyFloat(*P, Value);
	};
	auto Bool = [&](const TCHAR* Name, bool& Value)
	{
		const FWoTRuntimeWorldProperty* P = Defaults.FindClassProperty(Spec.Identity, Name);
		return P == nullptr || WoTRuntimeWorldPropertyBool(*P, Value);
	};
	auto Name = [&](const TCHAR* PropertyName, FName& Value)
	{
		const FWoTRuntimeWorldProperty* P =
			Defaults.FindClassProperty(Spec.Identity, PropertyName);
		if (P == nullptr) return true;
		if (P->Type != EWoTRuntimeWorldPropertyType::Name
			|| P->NameValue.IsEmpty()) return false;
		Value = FName(*P->NameValue);
		return true;
	};
	if (!Float(TEXT("Spread"), Out.Spread) || !Float(TEXT("Volume"), Out.Volume)
		|| !Float(TEXT("MinVolume"), Out.MinVolume)
		|| !Float(TEXT("PrimeCount"), Out.PrimeCount)
		|| !Float(TEXT("VolumeScalePct"), Out.VolumeScalePct)
		|| !Float(TEXT("VisibilityRadius"), Out.VisibilityRadius)
		|| !Float(TEXT("VisibilityHeight"), Out.VisibilityHeight)
		|| !Float(TEXT("TimerDuration"), Out.TimerDuration)
		|| !Bool(TEXT("bInitiallyOn"), Out.bInitiallyOn)
		|| !Bool(TEXT("bOn"), Out.bOn)
		|| !Bool(TEXT("bLOSClip"), Out.bLOSClip)
		|| !Name(TEXT("InitialState"), Out.InitialState)
		|| Out.TimerDuration < 0.0f)
	{
		Error = FString::Printf(TEXT("weave sprayer '%s' scalar defaults are malformed"),
			*Spec.Identity); return false;
	}
	if (const FWoTRuntimeWorldProperty* P =
		Defaults.FindClassProperty(Spec.Identity, TEXT("Gravity"));
		P != nullptr && !WoTRuntimeWorldPropertyVector(*P, Out.Gravity))
	{
		Error = FString::Printf(TEXT("weave sprayer '%s' Gravity is malformed"),
			*Spec.Identity); return false;
	}
	if (const FWoTRuntimeWorldProperty* P =
		Defaults.FindClassProperty(Spec.Identity, TEXT("ParticleDistribution")))
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*P, Value)) return false;
		Out.bLinearDistribution = Value == 1;
	}
	return true;
}

bool WoTWeaveReadByte(const FWoTRuntimeClassDefaults& Defaults,
	const FString& Identity, const TCHAR* Name, float& Out)
{
	const FWoTRuntimeWorldProperty* P = Defaults.FindClassProperty(Identity, Name);
	if (P == nullptr) return false;
	uint8 Value = 0;
	if (!WoTRuntimeWorldPropertyByte(*P, Value)) return false;
	Out = static_cast<float>(Value); return true;
}

// These fields encode script-method behavior, not default-object data. Keep the
// small policy projection beside the decoder and cite the shipped methods that
// require it; all particle/material/default values still come from the source
// packages through FWoTRetailBuildContext.
void WoTWeaveApplyBehaviorPolicy(FName Config, FWoTSprayerBundleRow& Row)
{
	const FString Name = Config.ToString();
	if (Config == TEXT("SoulBarbSmoke"))
	{
		// SoulBarbSmoke.uc:24-29 rotates Gravity into actor space each tick.
		Row.bAlignGravityToRotation = true;
	}
	else if (Config == TEXT("MashadarPainSprayer"))
	{
		// MashadarPainSprayer.uc:15-20 hard-codes the one-shot actor life.
		Row.SelfDestroySeconds = 1.0f;
	}
	else if (Config == TEXT("MyrdSprayerExp"))
	{
		// MyrdSprayerExp.uc:15-30 emits for TimerDuration, then lets the
		// particles drain for the remainder of Duration.
		Row.BurstSeconds = 0.2f;
		Row.SelfDestroySeconds = 5.1f;
	}
	else if (Name.StartsWith(TEXT("LegionStompSprayer")))
	{
		// LegionStompSprayer{A-D}.uc:18-29 copies ActualLifeSpan and stops
		// emission when Duration expires. Both scalars are decoded defaults.
		if (Config == TEXT("LegionStompSprayerA")) Row.SelfDestroySeconds = 2.25f;
		else if (Config == TEXT("LegionStompSprayerB")) Row.SelfDestroySeconds = 2.15f;
		else if (Config == TEXT("LegionStompSprayerC")) Row.SelfDestroySeconds = 1.5f;
		else Row.SelfDestroySeconds = 2.75f;
		Row.BurstSeconds = 0.75f;
		if (Config == TEXT("LegionStompSprayerA"))
		{
			// LegionStompSprayerA.uc:31-36 fades its authored light over life.
			Row.LightProgram = EWoTSprayerLightProgram::LinearFade;
			Row.LightDuration = 2.25f;
		}
	}
	else if (Config == TEXT("WaterSplash"))
	{
		// WaterSplash.uc:10-21 stops emission and then lingers.
		Row.BurstSeconds = 0.2f;
		Row.SelfDestroySeconds = 2.2f;
	}
	else if (Config == TEXT("BlastBits"))
	{
		// BlastBits.uc:17-29 defines its burst and actor lifetime.
		Row.BurstSeconds = 0.2f;
		Row.SelfDestroySeconds = 2.0f;
	}
	else if (Config == TEXT("AppearSprayerWhite"))
	{
		// AppearEffect.uc:94-116 gives the white endpoint its special burst/linger
		// choreography; colored variants remain owner-controlled.
		Row.BurstSeconds = 1.5f;
		Row.SelfDestroySeconds = 3.5f;
	}
	else if (Config == TEXT("LavaSpew"))
	{
		// LavaSpew.uc:18-35 projects Duration/ActualLifeSpan into lifecycle.
		Row.BurstSeconds = 0.25f;
		Row.SelfDestroySeconds = 3.5f;
	}
	else if (Config == TEXT("WOTSparks"))
	{
		// WOTSparks.uc:20-27 plus TriggerTimed defaults.
		Row.BurstSeconds = 0.25f;
		Row.SelfDestroySeconds = 1.5f;
	}
	else if (Name.StartsWith(TEXT("Firework05")))
	{
		// Firework05.uc:17-38 and :41-71.
		Row.BurstSeconds = 0.4f; Row.SelfDestroySeconds = 3.0f;
		Row.LightProgram = EWoTSprayerLightProgram::LinearFade;
		Row.LightDuration = 3.0f;
	}
	else if (Name.StartsWith(TEXT("Firework07")))
	{
		// Firework07.uc:20-75 and :78-108.
		Row.BurstSeconds = 0.25f; Row.SelfDestroySeconds = 4.25f;
		Row.LightProgram = EWoTSprayerLightProgram::Firework07Stages;
		Row.LightDuration = 4.25f; Row.RotationYawRate = 50000.0f;
	}
	else if (Name.StartsWith(TEXT("Firework08")))
	{
		// Firework08.uc:17-38 and :41-71.
		Row.BurstSeconds = 0.25f; Row.SelfDestroySeconds = 4.25f;
		Row.LightProgram = EWoTSprayerLightProgram::LinearFade;
		Row.LightDuration = 4.25f;
	}
	else if (Name.StartsWith(TEXT("Firework09")))
	{
		// Firework09.uc:17-39 and :42-73.
		Row.BurstSeconds = 0.25f; Row.SelfDestroySeconds = 3.25f;
		Row.LightProgram = EWoTSprayerLightProgram::LinearFade;
		Row.LightDuration = 3.25f; Row.RotationYawRate = 16384.0f;
	}
	else if (Config == TEXT("BFExpSprayer"))
	{
		// BFExpSprayer.uc:25-74: delayed burst, light fade, and rotating
		// template groups. The rotator values use the original UE1 units.
		Row.BurstSeconds = 0.3f; Row.SelfDestroySeconds = 1.8f;
		Row.LightRegistryKey = TEXT("BFExpSprayer");
		Row.LightProgram = EWoTSprayerLightProgram::LinearFade;
		Row.LightDuration = 1.8f;
		Row.RotationRate = FRotator(150000.0f, 100000.0f, 170000.0f);
		Row.RotationGroupDelay = 1.0f;
	}
	else if (Name.StartsWith(TEXT("PixieSprayer")))
	{
		// PixieJar.uc:71-103 selects a color leaf while preserving the base
		// PixieSprayer light shape and replacing only its hue.
		Row.LightRegistryKey = TEXT("PixieSprayer");
		Row.bOverrideLightHue = true;
	}
}

bool WoTWeaveDecodeBundle(const FWoTRetailBuildContext& Context,
	const FWoTWeaveBundleSpec& Spec, FWoTSprayerBundleRow& Row, FString& Error)
{
	FWoTRuntimeParticleSprayerConfiguration Particle;
	if (!WoTWeaveReadSprayer(Context.ClassDefaults, Spec, Particle, Error))
		return false;
	Row.Spread = Particle.Spread; Row.Volume = Particle.Volume;
	Row.MinVolume = Particle.MinVolume; Row.VolumeScalePct = Particle.VolumeScalePct;
	Row.VisibilityRadius = Particle.VisibilityRadius;
	Row.VisibilityHeight = Particle.VisibilityHeight; Row.Gravity = Particle.Gravity;
	Row.InitialState = Particle.InitialState;
	Row.TimerDuration = Particle.TimerDuration;
	Row.bInitiallyOn = Particle.bInitiallyOn;
	Row.bOn = Particle.bOn;
	Row.bLOSClip = Particle.bLOSClip;
	Row.bLinearDistribution = Particle.bLinearDistribution;
	Row.PrimeCount = Particle.PrimeCount;
	Row.Templates.Reset();
	for (int32 Index = 0; Index < Particle.Templates.Num(); ++Index)
	{
		const FString& TextureIdentity = Particle.TextureIdentities[Index];
		FWoTRetailTextureFacts Facts;
		if (!Context.FindTexture || !Context.FindTexture(TextureIdentity, Facts))
		{
			Error = FString::Printf(TEXT("weave sprayer '%s' texture '%s' is unavailable"),
				*Spec.Identity, *TextureIdentity); return false;
		}
		FWoTSprayerTemplateRow& Target = Row.Templates.AddDefaulted_GetRef();
		Target.Template = Particle.Templates[Index];
		if (Spec.TrapInfoIndex != INDEX_NONE && Index == 1)
		{
			const FWoTRuntimeWorldProperty* Info =
				Context.ClassDefaults.FindClassProperty(TEXT("Angreal.TrapDetectLeech"),
					TEXT("TrapIconInfos"), static_cast<uint32>(Spec.TrapInfoIndex));
			if (Info == nullptr || Info->Type != EWoTRuntimeWorldPropertyType::Struct)
			{
				Error = FString::Printf(TEXT("TrapIconInfos[%d] is missing or malformed"),
					Spec.TrapInfoIndex); return false;
			}
			auto MemberFloat = [&](const TCHAR* Name, float& Value)
			{
				const FWoTRuntimeWorldProperty* Member =
					WoTRuntimeWorldStructMember(*Info, Name);
				if (Member == nullptr) { Value = 0.0f; return true; }
				return WoTRuntimeWorldPropertyFloat(*Member, Value);
			};
			auto MemberByte = [&](const TCHAR* Name, uint8& Value)
			{
				const FWoTRuntimeWorldProperty* Member =
					WoTRuntimeWorldStructMember(*Info, Name);
				if (Member == nullptr) { Value = 0; return true; }
				return WoTRuntimeWorldPropertyByte(*Member, Value);
			};
			FWoTParticleTemplate& T = Target.Template;
			if (!MemberFloat(TEXT("Weight"), T.Weight)
				|| !MemberFloat(TEXT("MaxInitialVelocity"), T.MaxInitialVelocity)
				|| !MemberFloat(TEXT("MinInitialVelocity"), T.MinInitialVelocity)
				|| !MemberFloat(TEXT("MaxDrawScale"), T.MaxDrawScale)
				|| !MemberFloat(TEXT("MinDrawScale"), T.MinDrawScale)
				|| !MemberFloat(TEXT("MaxScaleGlow"), T.MaxScaleGlow)
				|| !MemberFloat(TEXT("MinScaleGlow"), T.MinScaleGlow)
				|| !MemberByte(TEXT("GrowPhase"), T.GrowPhase)
				|| !MemberFloat(TEXT("MaxGrowRate"), T.MaxGrowRate)
				|| !MemberFloat(TEXT("MinGrowRate"), T.MinGrowRate)
				|| !MemberByte(TEXT("FadePhase"), T.FadePhase)
				|| !MemberFloat(TEXT("MaxFadeRate"), T.MaxFadeRate)
				|| !MemberFloat(TEXT("MinFadeRate"), T.MinFadeRate))
			{
				Error = FString::Printf(TEXT("TrapIconInfos[%d] particle members are malformed"),
					Spec.TrapInfoIndex); return false;
			}
		}
		Target.Template.TexWidth = static_cast<float>(Facts.USize);
		Target.Template.TexHeight = static_cast<float>(Facts.VSize);
		Target.MaterialSlug = FName(*WoTWeaveObjectSlug(
			TextureIdentity, Facts.bProcedural));
	}
	if (const FWoTRuntimeWorldProperty* Style =
		Context.ClassDefaults.FindClassProperty(Spec.Identity, TEXT("Style")))
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*Style, Value))
		{
			Error = FString::Printf(TEXT("weave sprayer '%s' Style is malformed"),
				*Spec.Identity); return false;
		}
		Row.bModulateOnly = Value == 4;
	}
	const FWoTRuntimeWorldProperty* Rotation = nullptr;
	TArray<FString> RotationChain;
	Context.ClassDefaults.GetClassChain(Spec.Identity, RotationChain);
	for (const FString& Identity : RotationChain)
	{
		if (Identity.EndsWith(TEXT(".ParticleSprayer"), ESearchCase::IgnoreCase))
			break;
		Rotation = Context.ClassDefaults.FindOwnClassProperty(
			Identity, TEXT("Rotation"));
		if (Rotation != nullptr) break;
	}
	// Firework02.uc:21-34 drives its roll from the actor rotation while leaving
	// Rotation itself inherited from ParticleSprayer.uc:438.
	if (Rotation == nullptr
		&& Spec.Name.ToString().StartsWith(TEXT("Firework02")))
	{
		Rotation = Context.ClassDefaults.FindOwnClassProperty(
			TEXT("ParticleSystems.ParticleSprayer"), TEXT("Rotation"));
	}
	if (Rotation != nullptr)
	{
		if (!WoTRuntimeWorldPropertyRotator(*Rotation, Row.DefaultRotation))
		{
			Error = FString::Printf(TEXT("weave sprayer '%s' Rotation is malformed"),
				*Spec.Identity); return false;
		}
	}
	float LightType = 0.0f;
	if (WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightType"),
		LightType)) Row.bHasLight = LightType != 0.0f;
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightBrightness"),
		Row.LightBrightness);
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightHue"),
		Row.LightHue);
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightSaturation"),
		Row.LightSaturation);
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightRadius"),
		Row.LightRadius);
	return true;
}

struct FWoTExplosionSpec { const TCHAR* Name; const TCHAR* Identity; };
constexpr FWoTExplosionSpec GWoTExplosions[] = {
	{TEXT("FireballExplode3"), TEXT("Angreal.FireballExplode3")},
	{TEXT("FireballExplode"), TEXT("Angreal.FireballExplode")},
	{TEXT("FireballExplode2"), TEXT("Angreal.FireballExplode2")},
	{TEXT("GreenExplode"), TEXT("Angreal.GreenExplode")},
	{TEXT("DecayExplosion"), TEXT("Angreal.DecayExplosion")},
	{TEXT("FireballFizzle"), TEXT("Angreal.FireballFizzle")},
	{TEXT("BFExplodeA"), TEXT("Angreal.BFExplodeA")},
	{TEXT("MyrddraalElectric"), TEXT("WoTPawns.MyrddraalElectric")},
};

bool WoTWeaveDecodeExplosion(const FWoTRetailBuildContext& Context,
	const FWoTExplosionSpec& Spec, FWoTExplosionRow& Row, FString& Error)
{
	TArray<FString> Chain;
	if (!Context.ClassDefaults.GetClassChain(Spec.Identity, Chain))
	{
		Error = FString::Printf(TEXT("explosion class '%s' is missing"), Spec.Identity);
		return false;
	}
	TArray<FString> Frames;
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		const FWoTRuntimeWorldProperty* P = Context.ClassDefaults.FindClassProperty(
			Spec.Identity, TEXT("ExplosionAnim"), Index);
		if (P == nullptr) break;
		if (P->Type != EWoTRuntimeWorldPropertyType::Object
			|| P->ObjectIdentity.IsEmpty())
		{
			Error = FString::Printf(TEXT("explosion '%s' frame %u is malformed"),
				Spec.Identity, Index); return false;
		}
		Frames.Add(P->ObjectIdentity);
	}
	if (Frames.IsEmpty())
	{
		Error = FString::Printf(TEXT("explosion '%s' has no frames"), Spec.Identity);
		return false;
	}
	const FString FirstLeaf = WoTWeaveLeaf(Frames[0]).ToString();
	FWoTRetailObjectPath FirstPath;
	if (!WoTParseRetailObjectPath(Frames[0], FirstPath))
	{
		Error = FString::Printf(TEXT("explosion '%s' frame identity is malformed"),
			Spec.Identity); return false;
	}
	Row.FramePackage = FName(*FirstPath.Package);
	int32 Digits = FirstLeaf.Len();
	while (Digits > 0 && FChar::IsDigit(FirstLeaf[Digits - 1])) --Digits;
	const FString Number = FirstLeaf.RightChop(Digits);
	Row.FramePrefix = FName(*FirstLeaf.Left(Digits + FMath::Max(0, Number.Len() - 2)));
	Row.FrameStart = FCString::Atoi(*Number.Right(2));
	Row.FrameCount = Frames.Num();
	FWoTRetailTextureFacts Facts;
	if (!Context.FindTexture || !Context.FindTexture(Frames[0], Facts))
	{
		Error = FString::Printf(TEXT("explosion '%s' texture '%s' is unavailable"),
			Spec.Identity, *Frames[0]); return false;
	}
	Row.TexSize = static_cast<float>(Facts.USize);
	auto Float = [&](const TCHAR* Name, float& Value)
	{
		const FWoTRuntimeWorldProperty* P =
			Context.ClassDefaults.FindClassProperty(Spec.Identity, Name);
		return P != nullptr && WoTRuntimeWorldPropertyFloat(*P, Value);
	};
	if (!Float(TEXT("LifeSpan"), Row.LifeSpan)
		|| !Float(TEXT("DrawScale"), Row.DrawScale))
	{
		Error = FString::Printf(TEXT("explosion '%s' scalar defaults are malformed"),
			Spec.Identity); return false;
	}
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightBrightness"),
		Row.LightBrightness);
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightHue"),
		Row.LightHue);
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightSaturation"),
		Row.LightSaturation);
	WoTWeaveReadByte(Context.ClassDefaults, Spec.Identity, TEXT("LightRadius"),
		Row.LightRadius);
	if (const FWoTRuntimeWorldProperty* Effect =
		Context.ClassDefaults.FindClassProperty(Spec.Identity, TEXT("LightEffect")))
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*Effect, Value))
		{
			Error = FString::Printf(TEXT("explosion '%s' LightEffect is malformed"),
				Spec.Identity); return false;
		}
		Row.bNonIncidence = Value == 13;
	}
	if (Spec.Identity == FStringView(TEXT("Angreal.BFExplodeA")))
	{
		// BFExplodeA.uc:40-71 is the grow/sustain/shrink tick program; its
		// two authored phase ratios live in the class defaults.
		if (!Float(TEXT("StopGrowPct"), Row.StopGrowPct)
			|| !Float(TEXT("ShrinkPct"), Row.ShrinkPct))
		{
			Error = TEXT("BFExplodeA phase defaults are malformed"); return false;
		}
		Row.bGrowSustainShrink = true;
	}
	return true;
}

void WoTWeaveSerializeTemplate(TArray<uint8>& Out,
	const FWoTSprayerTemplateRow& Entry)
{
	using namespace WoTRetailBuilderCanonical;
	Name(Out, Entry.MaterialSlug);
	const FWoTParticleTemplate& T = Entry.Template;
	for (float Value : {T.LifeSpan, T.Weight, T.MaxInitialVelocity,
		T.MinInitialVelocity, T.MaxDrawScale, T.MinDrawScale, T.MaxScaleGlow,
		T.MinScaleGlow}) { Ascii(Out, " "); Float(Out, Value); }
	Ascii(Out, " "); Integer(Out, T.GrowPhase);
	for (float Value : {T.MaxGrowRate, T.MinGrowRate})
	{ Ascii(Out, " "); Float(Out, Value); }
	Ascii(Out, " "); Integer(Out, T.FadePhase);
	for (float Value : {T.MaxFadeRate, T.MinFadeRate, T.TexWidth, T.TexHeight})
	{ Ascii(Out, " "); Float(Out, Value); }
}
}

bool FWoTWeaveFXBuilder::Build(const FWoTRetailBuildContext& Context,
	FWoTWeaveFXSet& Out, FString& OutError)
{
	if (!Context.ClassDefaults.IsLoaded())
	{
		OutError = TEXT("weave-fx requires loaded class defaults"); return false;
	}
	FWoTWeaveFXSet Candidate;
	const TArray<FWoTWeaveBundleSpec> Specs = WoTWeaveSpecs();
	if (Specs.Num() != 84)
	{
		OutError = TEXT("weave-fx census is not 84 sprayer bundles"); return false;
	}
	for (const FWoTWeaveBundleSpec& Spec : Specs)
	{
		FWoTSprayerBundleRow* Row = &Candidate.Bundles.Add(Spec.Name);
		WoTWeaveApplyBehaviorPolicy(Spec.Name, *Row);
		if (!WoTWeaveDecodeBundle(Context, Spec, *Row, OutError)) return false;
	}
	for (const FWoTExplosionSpec& Spec : GWoTExplosions)
	{
		FWoTExplosionRow* Row = &Candidate.Explosions.Add(Spec.Name);
		if (!WoTWeaveDecodeExplosion(Context, Spec, *Row, OutError))
			return false;
	}
	Out = MoveTemp(Candidate); OutError.Reset(); return true;
}

#if WITH_AUTOMATION_TESTS
bool FWoTWeaveFXBuilder::DecodeBundleForTesting(
	const FWoTRetailBuildContext& Context, FName Name,
	const FString& Identity, FWoTSprayerBundleRow& Out, FString& OutError)
{
	const FWoTWeaveBundleSpec Spec{Name, Identity, {}};
	FWoTSprayerBundleRow Candidate;
	if (!WoTWeaveDecodeBundle(Context, Spec, Candidate, OutError)) return false;
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}
#endif

void FWoTWeaveFXBuilder::SerializeCanonical(const FWoTWeaveFXSet& Set,
	TArray<uint8>& Out)
{
	using namespace WoTRetailBuilderCanonical;
	Out.Reset(); Ascii(Out, "wot-weave-fx-v2\nbundles ");
	Integer(Out, Set.Bundles.Num()); Ascii(Out, "\n");
	TArray<FName> Names; Set.Bundles.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		const FWoTSprayerBundleRow& R = Set.Bundles.FindChecked(NameValue);
		Ascii(Out, "bundle "); Name(Out, NameValue);
		for (float Value : {R.Spread, R.Volume, R.MinVolume, R.VolumeScalePct,
			R.VisibilityRadius, R.VisibilityHeight, static_cast<float>(R.Gravity.X),
			static_cast<float>(R.Gravity.Y), static_cast<float>(R.Gravity.Z), R.PrimeCount})
		{ Ascii(Out, " "); Float(Out, Value); }
		for (bool Value : {R.bAlignGravityToRotation, R.bLOSClip, R.bModulateOnly,
			R.bLinearDistribution, R.bInitiallyOn, R.bOn})
		{ Ascii(Out, " "); Bool(Out, Value); }
		Ascii(Out, " "); Name(Out, R.InitialState); Ascii(Out, " ");
		Float(Out, R.TimerDuration);
		for (float Value : {static_cast<float>(R.DefaultRotation.Pitch),
			static_cast<float>(R.DefaultRotation.Yaw), static_cast<float>(R.DefaultRotation.Roll)})
		{ Ascii(Out, " "); Float(Out, Value); }
		Ascii(Out, " "); Bool(Out, R.bHasLight); Ascii(Out, " ");
		Name(Out, R.LightRegistryKey); Ascii(Out, " "); Bool(Out, R.bOverrideLightHue);
		for (float Value : {R.LightBrightness, R.LightHue, R.LightSaturation,
			R.LightRadius, R.LightDuration, R.RotationYawRate,
			static_cast<float>(R.RotationRate.Pitch),
			static_cast<float>(R.RotationRate.Yaw),
			static_cast<float>(R.RotationRate.Roll), R.RotationGroupDelay,
			R.SelfDestroySeconds, R.BurstSeconds})
		{ Ascii(Out, " "); Float(Out, Value); }
		Ascii(Out, " "); Integer(Out, static_cast<uint8>(R.LightProgram));
		Ascii(Out, " templates "); Integer(Out, R.Templates.Num()); Ascii(Out, "\n");
		for (const FWoTSprayerTemplateRow& T : R.Templates)
		{ Ascii(Out, "template "); WoTWeaveSerializeTemplate(Out, T); Ascii(Out, "\n"); }
	}
	Ascii(Out, "explosions "); Integer(Out, Set.Explosions.Num()); Ascii(Out, "\n");
	Names.Reset(); Set.Explosions.GetKeys(Names); Names.Sort(NameLess);
	for (FName NameValue : Names)
	{
		const FWoTExplosionRow& R = Set.Explosions.FindChecked(NameValue);
		Ascii(Out, "explosion "); Name(Out, NameValue); Ascii(Out, " ");
		Name(Out, R.FramePackage); Ascii(Out, " ");
		Name(Out, R.FramePrefix); Ascii(Out, " "); Integer(Out, R.FrameStart);
		Ascii(Out, " "); Integer(Out, R.FrameCount);
		for (float Value : {R.LifeSpan, R.DrawScale, R.TexSize, R.LightBrightness,
			R.LightHue, R.LightSaturation, R.LightRadius})
		{ Ascii(Out, " "); Float(Out, Value); }
		Ascii(Out, " "); Bool(Out, R.bGrowSustainShrink);
		Ascii(Out, " "); Float(Out, R.StopGrowPct); Ascii(Out, " ");
		Float(Out, R.ShrinkPct); Ascii(Out, " "); Bool(Out, R.bNonIncidence);
		Ascii(Out, "\n");
	}
}

FString FWoTWeaveFXBuilder::CanonicalDigest(const FWoTWeaveFXSet& Set)
{
	TArray<uint8> Bytes; SerializeCanonical(Set, Bytes); return Sha256(Bytes).ToHex();
}

bool FWoTWeaveFXBuilder::AreEquivalent(const FWoTWeaveFXSet& Built,
	const FWoTWeaveFXSet& Expected, FString& OutError)
{
	TArray<uint8> A; TArray<uint8> B;
	SerializeCanonical(Built, A); SerializeCanonical(Expected, B);
	if (A != B)
	{
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
					OutError += FString::Printf(TEXT("weave-fx canonical line %d differs: built '%s', expected '%s'\n"),
						Index + 1, *ALines[Index], *BLines[Index]);
				}
			}
		}
		if (DifferenceCount > 0) return false;
		OutError = FString::Printf(TEXT("weave-fx canonical line count differs: built %d, expected %d"),
			ALines.Num(), BLines.Num());
		return false;
	}
	OutError.Reset(); return true;
}
