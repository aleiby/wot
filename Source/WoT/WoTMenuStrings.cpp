#include "WoTMenuStrings.h"

FText UWoTMenuStrings::Resolve(FName Id) const
{
	if (const FString* Value = Strings.Find(Id))
	{
		return FText::FromString(*Value);
	}
	return FText::FromName(Id);
}
