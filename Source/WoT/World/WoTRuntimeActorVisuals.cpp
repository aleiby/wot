#include "WoTRuntimeActorVisuals.h"

#include "WoTRuntimeClassDefaults.h"
#include "../Assets/WoTRetailIdentity.h"
#include "WoTRuntimeWorldProperties.h"
#include "../WoTVertexAnimComponent.h"

namespace
{
bool SplitObjectIdentity(const FWoTRuntimeWorldProperty* Property,
	FName& OutPackage, FName& OutName, FString& OutError)
{
	if (Property == nullptr || Property->ObjectIdentity.IsEmpty()) return true;
	if (Property->Type != EWoTRuntimeWorldPropertyType::Object)
	{
		OutError = TEXT("a mesh presentation object property has the wrong type");
		return false;
	}
	TArray<FString> Pieces;
	Property->ObjectIdentity.ParseIntoArray(Pieces, TEXT("."), true);
	FString ObjectName = Pieces.IsEmpty() ? FString() : Pieces.Last();
	const int32 ExportMarker = ObjectName.Find(
		TEXT("#export-"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ExportMarker > 0)
	{
		const FString ExportIndex = ObjectName.Mid(ExportMarker + 8);
		if (!ExportIndex.IsEmpty() && ExportIndex.IsNumeric())
		{
			ObjectName.LeftInline(ExportMarker);
		}
	}
	if (Pieces.Num() < 2 || Pieces[0].IsEmpty() || ObjectName.IsEmpty()
		|| Pieces[0].Len() >= NAME_SIZE || ObjectName.Len() >= NAME_SIZE)
	{
		OutError = TEXT("a mesh presentation object identity is malformed");
		return false;
	}
	OutPackage = FName(*Pieces[0]);
	OutName = FName(*ObjectName);
	return true;
}

bool ReadFloat(const FWoTRuntimeWorldProperty* Property, float& Out)
{
	if (Property == nullptr) return true;
	if (WoTRuntimeWorldPropertyFloat(*Property, Out)) return true;
	int32 Integer = 0;
	if (!WoTRuntimeWorldPropertyInteger(*Property, Integer)) return false;
	Out = static_cast<float>(Integer);
	return true;
}

/** Skin texture soft path of a retail object identity: "Pkg/Name", or the
 * exact "Pkg/Group/.../Name" when the name collides inside its package and the
 * authenticated registry holds that identity (WoTRetailIdentity.h). */
FSoftObjectPath TexturePath(const FWoTRuntimeWorldProperty* Property,
	FName Package, FName Name)
{
	FWoTRetailObjectPath Path;
	if (Property != nullptr
		&& WoTParseRetailObjectPath(Property->ObjectIdentity, Path))
	{
		Path.Package = Package.ToString();
		Path.Name = Name.ToString();
		return FSoftObjectPath(FString::Printf(TEXT("/Game/WoT/Textures/%s.%s"),
			*WoTRetailTextureSubPath(Path), *Path.Name));
	}
	return FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/%s/%s.%s"),
		*Package.ToString(), *Name.ToString(), *Name.ToString()));
}
}

bool WoTRuntimeReadActorVisualPresentation(
	const FWoTRuntimeClassDefaults& Defaults,
	const FWoTRuntimeWorldActor& Actor,
	FWoTRuntimeActorVisualPresentation& Out, FString& OutError)
{
	Out = FWoTRuntimeActorVisualPresentation();
	OutError.Reset();
	const FWoTRuntimeWorldProperty* Mesh =
		Defaults.FindProperty(Actor, TEXT("Mesh"));
	if (!SplitObjectIdentity(Mesh, Out.MeshPackage, Out.MeshName, OutError))
		return false;
	if (!ReadFloat(Defaults.FindProperty(Actor, TEXT("DrawScale")), Out.DrawScale)
		|| !ReadFloat(Defaults.FindProperty(Actor, TEXT("ScaleGlow")), Out.ScaleGlow)
		|| !FMath::IsFinite(Out.DrawScale) || Out.DrawScale <= 0.0f
		|| !FMath::IsFinite(Out.ScaleGlow) || Out.ScaleGlow < 0.0f)
	{
		OutError = TEXT("a mesh presentation scale is malformed");
		return false;
	}
	if (const FWoTRuntimeWorldProperty* Ambient =
		Defaults.FindProperty(Actor, TEXT("AmbientGlow")))
	{
		if (!WoTRuntimeWorldPropertyByte(*Ambient, Out.AmbientGlow))
		{
			OutError = TEXT("a mesh presentation AmbientGlow is malformed");
			return false;
		}
	}
	if (const FWoTRuntimeWorldProperty* Fatness =
		Defaults.FindProperty(Actor, TEXT("Fatness")))
	{
		if (!WoTRuntimeWorldPropertyByte(*Fatness, Out.Fatness))
		{
			OutError = TEXT("a mesh presentation Fatness is malformed");
			return false;
		}
	}
	if (const FWoTRuntimeWorldProperty* Unlit =
		Defaults.FindProperty(Actor, TEXT("bUnlit")))
	{
		if (!WoTRuntimeWorldPropertyBool(*Unlit, Out.bUnlit))
		{
			OutError = TEXT("a mesh presentation bUnlit is malformed");
			return false;
		}
	}
	if (const FWoTRuntimeWorldProperty* Curvy =
		Defaults.FindProperty(Actor, TEXT("bMeshCurvy")))
	{
		if (!WoTRuntimeWorldPropertyBool(*Curvy, Out.bMeshCurvy))
		{
			OutError = TEXT("a mesh presentation bMeshCurvy value is malformed");
			return false;
		}
	}
	// Retail shipped CurvedSurfaces=False, so nothing rendered curved; the port
	// rounds the low-poly spheres that carry the flag (PORT_NOTES, curved
	// vertex-mesh presentation). AngrealInvAMA also flags its pickup
	// (AngrealInvAMA.uc:56), a 12-triangle box that would round into a blob,
	// so it stays a box.
	if (Out.MeshPackage == FName(TEXT("Angreal"))
		&& Out.MeshName == FName(TEXT("AngrealAntiMagicAuraPickup")))
	{
		Out.bMeshCurvy = false;
	}
	if (const FWoTRuntimeWorldProperty* Style =
		Defaults.FindProperty(Actor, TEXT("Style")))
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*Style, Value) || Value > 4)
		{
			OutError = TEXT("a mesh presentation Style is malformed");
			return false;
		}
		Out.Style = Value; // Engine.Actor.ERenderStyle
	}
	Out.bStyleTranslucent = Out.Style == 3;
	if (const FWoTRuntimeWorldProperty* Rotating =
		Defaults.FindProperty(Actor, TEXT("bRotatingPickup")))
	{
		if (!WoTRuntimeWorldPropertyBool(*Rotating, Out.bRotatingPickup))
		{
			OutError = TEXT("a pickup bRotatingPickup value is malformed");
			return false;
		}
	}
	if (const FWoTRuntimeWorldProperty* Rotation =
		Defaults.FindProperty(Actor, TEXT("RotationRate")))
	{
		if (!WoTRuntimeWorldPropertyRotator(*Rotation, Out.RotationRate))
		{
			OutError = TEXT("a pickup RotationRate is malformed");
			return false;
		}
	}

	FName SkinPackage;
	FName SkinName;
	const FWoTRuntimeWorldProperty* SkinProperty =
		Defaults.FindProperty(Actor, TEXT("Skin"));
	if (!SplitObjectIdentity(SkinProperty, SkinPackage, SkinName, OutError)) return false;
	if (!SkinName.IsNone()) Out.Skin =
		TexturePath(SkinProperty, SkinPackage, SkinName);
	for (uint32 Slot = 0; Slot < 8; ++Slot)
	{
		FName Package;
		FName Name;
		const FWoTRuntimeWorldProperty* MultiSkin =
			Defaults.FindProperty(Actor, TEXT("MultiSkins"), Slot);
		if (!SplitObjectIdentity(MultiSkin, Package, Name, OutError)) return false;
		if (!Name.IsNone()) Out.SkinOverrides.Add(static_cast<int32>(Slot),
			TexturePath(MultiSkin, Package, Name));
	}
	return true;
}

bool WoTRuntimeReadClassVisualPresentation(
	const FWoTRuntimeClassDefaults& Defaults, FStringView ClassIdentity,
	FWoTRuntimeActorVisualPresentation& Out, FString& OutError)
{
	FWoTRuntimeWorldActor Actor;
	Actor.ClassIdentity = FString(ClassIdentity);
	return WoTRuntimeReadActorVisualPresentation(Defaults, Actor, Out, OutError);
}

namespace
{
bool ApplyBoundActorVisualPresentation(
	UWoTVertexAnimComponent& Component,
	const FWoTRuntimeActorVisualPresentation& Presentation,
	FString& OutError)
{
	Component.SetRelativeScale3D(FVector(Presentation.DrawScale));
	Component.SetFatness(Presentation.Fatness);
	Component.SetActorSkin(Presentation.Skin);
	for (const TPair<int32, FSoftObjectPath>& Pair : Presentation.SkinOverrides)
	{
		if (!Component.SetSkinOverride(Pair.Key, Pair.Value))
		{
			OutError = FString::Printf(TEXT("skin override slot %d could not be bound"),
				Pair.Key);
			return false;
		}
	}
	return true;
}
}

bool WoTRuntimeApplyActorVisualPresentation(
	UWoTVertexAnimComponent& Component,
	const FWoTRuntimeActorVisualPresentation& Presentation,
	FString& OutError)
{
	OutError.Reset();
	if (!Presentation.HasMesh())
	{
		OutError = TEXT("the retail class has no effective Mesh property");
		return false;
	}
	Component.bMeshCurvy = Presentation.bMeshCurvy;
	Component.Style = Presentation.Style;
	// Always bind after applying the actor flag. A generated dev BP may already
	// carry the flat mesh, and HasDisplayMesh alone cannot distinguish variants.
	if (!Component.SetVertexMesh(Presentation.MeshName, Presentation.MeshPackage))
	{
		OutError = FString::Printf(TEXT("mesh %s.%s could not be bound"),
			*Presentation.MeshPackage.ToString(), *Presentation.MeshName.ToString());
		return false;
	}
	return ApplyBoundActorVisualPresentation(Component, Presentation, OutError);
}

#if WITH_AUTOMATION_TESTS
bool WoTRuntimeApplyBoundActorVisualPresentationForTesting(
	UWoTVertexAnimComponent& Component,
	const FWoTRuntimeActorVisualPresentation& Presentation,
	FString& OutError)
{
	OutError.Reset();
	return ApplyBoundActorVisualPresentation(Component, Presentation, OutError);
}
#endif
