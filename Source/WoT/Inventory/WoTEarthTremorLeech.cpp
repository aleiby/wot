#include "WoTEarthTremorLeech.h"
#include "../WoTPlayerController.h"

#include "GameFramework/Pawn.h"

UWoTEarthTremorLeech::UWoTEarthTremorLeech()
{
	// EarthTremorLeech defaultproperties: bDisplayIcon=False (everything else
	// inherited from DecreaseHealthLeech — DamagePerHit 1, AffectResolution 0.2,
	// bDeleterious true).
	bDisplayIcon = false;
}

void UWoTEarthTremorLeech::AffectHost(int32 Iterations)
{
	if (APawn* Victim = Cast<APawn>(GetOwner()))
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Victim->GetController()))
		{
			PC->ClientShakeView(AffectResolution, 2500.0f, 0.0f);
		}
	}
	Super::AffectHost(Iterations);
}
