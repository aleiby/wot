#pragma once

#include "CoreMinimal.h"

class USceneComponent;

enum class EWoTFirstPersonHand : uint8 { Primary, Secondary, Count };

// Anchor sources supply a parent and grip; the shared model presenter only
// applies relative pose/motion. Additional hand sources need no model changes.
struct FWoTFirstPersonAnchor
{
	TWeakObjectPtr<USceneComponent> Parent;
	FName Socket = NAME_None;
	FTransform Grip = FTransform::Identity;
};
