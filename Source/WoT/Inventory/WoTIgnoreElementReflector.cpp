#include "WoTIgnoreElementReflector.h"

#include "../WoTAppearEffect.h"
#include "../WoTEvents.h"
#include "../WoTMeshFX.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTWeaveFX.h"

void UWoTIgnoreElementReflector::SpawnImpactFeedback()
{
	APawn* Host = Cast<APawn>(GetOwner());
	UWorld* World = GetWorld();
	if (!Host || !World || GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	WoTMeshFX::SpawnBurstMarker(*World, ImpactConfig, Host->GetActorLocation(),
		Host->GetActorRotation(), Host);
	WoTSound::PlayReplicated(*World, DeflectSoundPath, Host->GetActorLocation(),
		WoTSound::WorldSoundRadius(32));
}

void UWoTIgnoreElementReflector::OnInstalledFeedback()
{
	APawn* Host = Cast<APawn>(GetOwner());
	UWorld* World = GetWorld();
	if (!Host || !World || GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	if (UWoTEventSubsystem* Events = World->GetSubsystem<UWoTEventSubsystem>())
	{
		Events->FireEvent(TriggerEvent, Host, Host);
	}
	// IgnoreElementReflector.uc:48-58: the install rings — SetColors(PlayerColor)
	// for a player host, 'Green' for anyone else (the retail non-WOTPlayer arm).
	const FName Color = WoTPlayerColorFX::Resolve(Host, TEXT("Green"));
	AWoTAppearEffect::SpawnColored(*World, Host, Color, Color);
}

void UWoTIgnoreElementReflector::OnUnregister()
{
	if (APawn* Host = Cast<APawn>(GetOwner()))
	{
		if (UWorld* World = GetWorld(); World && GetOwnerRole() == ROLE_Authority)
		{
			if (UWoTEventSubsystem* Events = World->GetSubsystem<UWoTEventSubsystem>())
			{
				Events->FireUnEvent(TriggerEvent, Host, Host);
			}
		}
	}
	Super::OnUnregister();
}
