#include "WoTReflectorInstaller.h"

#include "WoTReflector.h"

#include "GameFramework/Pawn.h"

AWoTReflectorInstaller::AWoTReflectorInstaller()
{
	Duration = 20.0f;
	Priority = 20.0f;   // ReflectorInstaller AI priority
}

void AWoTReflectorInstaller::CastArtifact()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		return;
	}

	// InstallReflectorEffect inlined: install each reflector on the caster,
	// sharing Duration so they expire together (cf. the leech installers).
	for (const TSubclassOf<UWoTReflector>& ReflectorClass : ReflectorClasses)
	{
		if (!ReflectorClass)
		{
			continue;
		}
		UWoTReflector* Reflector = NewObject<UWoTReflector>(OwnerPawn, ReflectorClass);
		Reflector->SetSourceAngreal(this);
		Reflector->Lifespan = Duration;
		Reflector->Install(OwnerPawn);
	}

	Super::CastArtifact();   // sound + HUD hook
	UseCharge();
}
