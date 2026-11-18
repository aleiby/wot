#include "WoTReturnToSenderReflector.h"

#include "../WoTMeshFX.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"

#include "GameFramework/Pawn.h"

bool UWoTReturnToSenderReflector::BuildTouchReflectorDescription(
	FWoTTouchReflectorDescription& Out) const
{
	Out = FWoTTouchReflectorDescription{};
	Out.Kind = EWoTTouchReflectorKind::Reflect;
	Out.Priority = Priority;
	Out.InstallSeq = InstallSeq;
	for (const FName Type : UnAffectedTypes)
		if (!Type.IsNone()) Out.UnaffectedProjectileTypes.Add(Type);
	return true;
}

void UWoTReturnToSenderReflector::OnProjectileFeedback()
{
	APawn* Host = Cast<APawn>(GetOwner());
	UWorld* World = GetWorld();
	if (!Host || !World || GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	const FName Color = WoTPlayerColorFX::Resolve(Host, TEXT("Green"));
	const FName Config(*FString::Printf(TEXT("Reflect%s%s"), *Color.ToString(),
		UsesMottledFeedback() ? TEXT("Mottled") : TEXT("")));
	WoTMeshFX::SpawnPawnOverlayBurst(*World, Config, Host);
	// ReturnToSenderReflector.uc:90 Owner.PlaySound: the host's slot, the
	// engine's default radius.
	WoTSound::PlayReplicatedFrom(*Host, ReflectSoundPath());
}
