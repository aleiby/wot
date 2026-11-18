#pragma once

#include "CoreMinimal.h"
#include "WoTTouchReflectorDescription.generated.h"

// Compact replicated presentation description of the installed projectile-touch
// chain. Authority retains the live component behavior; simulated copies need
// only ordering, kind, affectability names, and the seeker destination gate so
// they can retire an incoming cosmetic without producing a false explosion.
UENUM()
enum class EWoTTouchReflectorKind : uint8
{
	Absorb,
	Reflect,
	Fork,
};

USTRUCT()
struct FWoTTouchReflectorDescription
{
	GENERATED_BODY()

	UPROPERTY() EWoTTouchReflectorKind Kind = EWoTTouchReflectorKind::Absorb;
	UPROPERTY() uint8 Priority = 0;
	UPROPERTY() uint64 InstallSeq = 0;
	UPROPERTY() TArray<FName> UnaffectedProjectileTypes;
};
