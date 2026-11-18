#include "WoTHUDFonts.h"

#include "Engine/Font.h"
#include "Payload/WoTPayloadActivation.h"
#include "Payload/WoTFrontEndFontOwner.h"

UFont* UWoTHUDFonts::Resolve(FName Id) const
{
#if WOT_GUARDED_BOOTSTRAP
	(void)Id;
	return nullptr;
#else
	if (const TSoftObjectPtr<UFont>* Font = Fonts.Find(Id))
	{
		return Font->LoadSynchronous();
	}
	return nullptr;
#endif
}

UFont* UWoTHUDFonts::ResolveRuntime(FName Id)
{
#if WOT_GUARDED_BOOTSTRAP || WITH_AUTOMATION_TESTS
	if (Id.IsNone() || !IsWoTPayloadBootstrapActive())
	{
		return nullptr;
	}
	const FWoTFrontEndFontOwner* Owner = FWoTFrontEndFontOwner::GetActive();
	return Owner ? Owner->ResolveFontObject(Id) : nullptr;
#else
	(void)Id;
	return nullptr;
#endif
}

UFont* UWoTHUDFonts::ResolveCanvasRuntime(FName Id)
{
#if WOT_GUARDED_BOOTSTRAP || WITH_AUTOMATION_TESTS
	if (Id.IsNone() || !IsWoTPayloadBootstrapActive())
	{
		return nullptr;
	}
	const FWoTFrontEndFontOwner* Owner = FWoTFrontEndFontOwner::GetActive();
	return Owner ? Owner->ResolveCanvasFontObject(Id) : nullptr;
#else
	(void)Id;
	return nullptr;
#endif
}
