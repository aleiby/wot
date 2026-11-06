#include "WoTNeutralResources.h"

bool FWoTNeutralResourceId::IsValid() const
{
	return !LogicalId.IsEmpty()
		&& LogicalId.StartsWith(TEXT("cache/"), ESearchCase::CaseSensitive)
		&& !LogicalId.Contains(TEXT(".."))
		&& !LogicalId.Contains(TEXT("\\"));
}

const TCHAR* LexToString(EWoTNeutralResourceKind Kind)
{
	switch (Kind)
	{
	case EWoTNeutralResourceKind::Texture: return TEXT("texture");
	case EWoTNeutralResourceKind::Sound: return TEXT("sound");
	case EWoTNeutralResourceKind::Music: return TEXT("music");
	case EWoTNeutralResourceKind::VertexMesh: return TEXT("mesh");
	case EWoTNeutralResourceKind::Animation: return TEXT("animation");
	case EWoTNeutralResourceKind::BSP: return TEXT("bsp");
	case EWoTNeutralResourceKind::World: return TEXT("world");
	case EWoTNeutralResourceKind::Font: return TEXT("font");
	case EWoTNeutralResourceKind::Procedural: return TEXT("procedural");
	case EWoTNeutralResourceKind::Registry: return TEXT("registry");
	case EWoTNeutralResourceKind::Localization: return TEXT("localization");
	case EWoTNeutralResourceKind::ClassDefaults: return TEXT("class_default");
	}
	return TEXT("unknown");
}

FString WoTNeutralResourceSchema(EWoTNeutralResourceKind Kind)
{
	if (Kind == EWoTNeutralResourceKind::ClassDefaults)
	{
		return TEXT("wot-class-default-v3");
	}
	return FString::Printf(TEXT("wot-%s-v3"), LexToString(Kind));
}
