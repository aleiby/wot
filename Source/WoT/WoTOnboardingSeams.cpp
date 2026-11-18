#include "WoTOnboardingSeams.h"

FWoTOnboardingSeams& FWoTOnboardingSeams::Get()
{
	static FWoTOnboardingSeams Seams;
	return Seams;
}

FString FWoTOnboardingSeams::ResolveText(const FString& File,
	const FString& Section, const FString& Property) const
{
	return ResolveRetailText ? ResolveRetailText(File, Section, Property)
	                         : FString();
}
