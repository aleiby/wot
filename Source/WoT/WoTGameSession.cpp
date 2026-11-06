#include "WoTGameSession.h"

#include "WoTGameMode.h"
#include "WoTRetailText.h"

#include "Kismet/GameplayStatics.h"

FString AWoTGameSession::ApproveLogin(const FString& Options)
{
	FString Error = Super::ApproveLogin(Options);
	if (Error == TEXT("Server full."))
	{
		const AWoTGameMode* Game = GetWorld()
			? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
		const FString PasswordToken =
			UGameplayStatics::ParseOption(Options, TEXT("PasswordToken"));
		if (Game && Game->IsCapacityOverridePassword(PasswordToken))
		{
			Error.Reset();
		}
		else
		{
			FWoTRetailText::EnsureLoaded();
			if (const FString* Retail = FWoTRetailText::FindSectionValue(
				TEXT("GameInfo"), TEXT("MaxedOutMessage")))
			{
				Error = *Retail;
			}
			else
			{
				Error = TEXT("The server has reached its player limit.");
			}
		}
	}
	return Error;
}
