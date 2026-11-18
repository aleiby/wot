#include "WoTExtensionSeams.h"

FWoTExtensionSeams& FWoTExtensionSeams::Get()
{
	static FWoTExtensionSeams Seams;
	return Seams;
}

FString FWoTExtensionSeams::ResolveText(const FString& File,
	const FString& Section, const FString& Property) const
{
	return ResolveRetailText ? ResolveRetailText(File, Section, Property)
	                         : FString();
}
