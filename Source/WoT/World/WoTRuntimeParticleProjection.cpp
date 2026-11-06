#include "WoTRuntimeParticleProjection.h"

#include "../WoTParticleSprayer.h"
#include "WoTRuntimeWorldProperties.h"

bool WoTRuntimeReadParticleTemplate(
	const FWoTRuntimeWorldProperty& Property, FWoTParticleTemplate& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("Particle"), ESearchCase::IgnoreCase)
		|| Property.Value.Num() != 50) return false;
	int32 Offset = 0;
	auto Float = [&](float& Value)
	{
		FMemory::Memcpy(&Value, Property.Value.GetData() + Offset, 4);
		Offset += 4;
		return FMath::IsFinite(Value);
	};
	if (!Float(Out.LifeSpan) || !Float(Out.Weight)
		|| !Float(Out.MaxInitialVelocity) || !Float(Out.MinInitialVelocity)
		|| !Float(Out.MaxDrawScale) || !Float(Out.MinDrawScale)
		|| !Float(Out.MaxScaleGlow) || !Float(Out.MinScaleGlow)) return false;
	Out.GrowPhase = Property.Value[Offset++];
	if (!Float(Out.MaxGrowRate) || !Float(Out.MinGrowRate)) return false;
	Out.FadePhase = Property.Value[Offset++];
	return Float(Out.MaxFadeRate) && Float(Out.MinFadeRate) && Offset == 50
		&& Out.LifeSpan > 0.0f && Out.Weight >= 0.0f
		&& Out.MinDrawScale >= 0.0f && Out.MaxDrawScale >= 0.0f;
}

bool WoTRuntimeReadParticleSprayerConfiguration(
	TFunctionRef<const FWoTRuntimeWorldProperty*(FStringView, uint32)> Find,
	FWoTRuntimeParticleSprayerConfiguration& Out)
{
	uint8 Count = 0;
	const FWoTRuntimeWorldProperty* CountProperty = Find(TEXT("NumTemplates"),0);
	if (!CountProperty || !WoTRuntimeWorldPropertyByte(*CountProperty,Count)
		|| Count == 0 || Count > 16) return false;
	Out.Templates.SetNum(Count); Out.TextureIdentities.SetNum(Count);
	for (uint32 Index=0;Index<Count;++Index)
	{
		const FWoTRuntimeWorldProperty* Template=Find(TEXT("Templates"),Index);
		const FWoTRuntimeWorldProperty* Particle=Find(TEXT("Particles"),Index);
		if(!Template || !Particle || !WoTRuntimeReadParticleTemplate(*Template,Out.Templates[Index])
			|| Particle->Type!=EWoTRuntimeWorldPropertyType::Object
			|| Particle->ObjectIdentity.IsEmpty()) return false;
		Out.TextureIdentities[Index]=Particle->ObjectIdentity;
	}
	auto Float=[&](const TCHAR* Name,float& Value)
	{
		const auto* P=Find(Name,0); return !P || WoTRuntimeWorldPropertyFloat(*P,Value);
	};
	auto Bool=[&](const TCHAR* Name,bool& Value)
	{
		const auto* P=Find(Name,0); return !P || WoTRuntimeWorldPropertyBool(*P,Value);
	};
	auto Byte=[&](const TCHAR* Name,uint8& Value)
	{
		const auto* P=Find(Name,0); return !P || WoTRuntimeWorldPropertyByte(*P,Value);
	};
	auto Name=[&](const TCHAR* PropertyName,FName& Value)
	{
		const auto* P=Find(PropertyName,0);
		if(!P)return true;
		if(P->Type!=EWoTRuntimeWorldPropertyType::Name||P->NameValue.IsEmpty())return false;
		Value=FName(*P->NameValue);return true;
	};
	FVector Location=FVector::ZeroVector; FRotator Rotation=FRotator::ZeroRotator;
	if(const auto* P=Find(TEXT("Location"),0))if(!WoTRuntimeWorldPropertyVector(*P,Location))return false;
	if(const auto* P=Find(TEXT("Rotation"),0))if(!WoTRuntimeWorldPropertyRotator(*P,Rotation))return false;
	Out.Transform=FTransform(Rotation,Location);
	if(!Float(TEXT("Spread"),Out.Spread)||!Float(TEXT("Volume"),Out.Volume)
		||!Float(TEXT("MinVolume"),Out.MinVolume)||!Float(TEXT("PrimeCount"),Out.PrimeCount)
		||!Float(TEXT("VolumeScalePct"),Out.VolumeScalePct)
		||!Float(TEXT("VisibilityRadius"),Out.VisibilityRadius)
		||!Float(TEXT("VisibilityHeight"),Out.VisibilityHeight)
		||!Float(TEXT("TimerDuration"),Out.TimerDuration)
		||!Byte(TEXT("Style"),Out.Style)
		||!Bool(TEXT("bInitiallyOn"),Out.bInitiallyOn)
		||!Bool(TEXT("bOn"),Out.bOn)
		||!Bool(TEXT("bLOSClip"),Out.bLOSClip)
		||!Name(TEXT("InitialState"),Out.InitialState))return false;
	if(const auto* P=Find(TEXT("Gravity"),0))if(!WoTRuntimeWorldPropertyVector(*P,Out.Gravity))return false;
	if(const auto* P=Find(TEXT("ParticleDistribution"),0))
	{
		uint8 Value=0;if(!WoTRuntimeWorldPropertyByte(*P,Value)||Value>1)return false;
		Out.bLinearDistribution=Value==1;
	}
	// ParticleSprayer.Spread is a full azimuth in retail.  Battle2_03's live
	// dragonfly cluster authors 360 degrees and is rotated by ActorRotator0.
	return Out.Spread>=0 && Out.Spread<=360
		&& Out.Volume>=0
		&& Out.Volume<=WoTRuntimeParticleProjectionLimits::MaximumVolume
		&& Out.MinVolume>=0
		&& Out.TimerDuration>=0
		&& Out.Style<=4
		&& Out.VisibilityRadius>=0 && Out.VisibilityHeight>=0;
}

void WoTRuntimeApplyParticleSprayerConfiguration(
	const FWoTRuntimeParticleSprayerConfiguration& Source,
	AWoTParticleSprayer& Target)
{
	Target.Spread=Source.Spread;Target.Volume=Source.Volume;Target.MinVolume=Source.MinVolume;
	Target.Gravity=Source.Gravity;Target.AuthoredGravity=Source.Gravity;
	Target.PrimeCount=Source.PrimeCount;Target.VolumeScalePct=Source.VolumeScalePct;
	Target.VisibilityRadius=Source.VisibilityRadius;Target.VisibilityHeight=Source.VisibilityHeight;
	Target.bModulateOnly=Source.Style==4;
	// ParticleSprayer.uc:250 PreBeginPlay: bOn = bInitiallyOn. The authored bOn
	// is only UnrealEd's preview, and its class default is false, so a map
	// sprayer always starts at bInitiallyOn (Arena_09's OilSpray nozzles and
	// the Battle portal-stone flares start off until triggered).
	Target.bInitiallyOn=Source.bInitiallyOn;Target.bOn=Source.bInitiallyOn;
	Target.TriggerSeconds=Source.TimerDuration;
	if(Source.InitialState==TEXT("TriggerToggle"))Target.TriggerMode=EWoTParticleTriggerMode::Toggle;
	else if(Source.InitialState==TEXT("TriggerControl"))Target.TriggerMode=EWoTParticleTriggerMode::Control;
	else if(Source.InitialState==TEXT("TriggerTimed"))Target.TriggerMode=EWoTParticleTriggerMode::Timed;
	else Target.TriggerMode=EWoTParticleTriggerMode::None;
	Target.bLOSClip=Source.bLOSClip;
	Target.bLinearDistribution=Source.bLinearDistribution;Target.Templates=Source.Templates;
}
