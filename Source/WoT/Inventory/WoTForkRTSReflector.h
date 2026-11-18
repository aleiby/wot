#pragma once

#include "CoreMinimal.h"
#include "WoTReturnToSenderReflector.h"
#include "WoTForkRTSReflector.generated.h"

// Port of Angreal.ForkRTSReflector (expands ReturnToSenderReflector) — the Fork
// effect. Spec: exports/script/Angreal/ForkRTSReflector.uc. A weave that strikes
// the host still affects it, but a duplicate is launched back at the originator.
// A marker reflector: the fork lives in AWoTGenericProjectile::ProcessTouch's
// ordered reflector walk — the original falls through to
// hit the host, and a reversed/reseeking copy is spawned toward the sender. The
// bIsForkCopy flag stops a copy from forking again.
//
// The production fork path emits the mottled ReflectSkinEffect + Fork sound.
UCLASS()
class UWoTForkRTSReflector : public UWoTReturnToSenderReflector
{
	GENERATED_BODY()

public:
	UWoTForkRTSReflector()
	{
		Priority = 150;
		bRemoveExisting = true;
	}

	virtual bool ReflectsProjectiles() const override { return false; }
	virtual bool ForksProjectiles() const override { return true; }
	virtual bool BuildTouchReflectorDescription(
		FWoTTouchReflectorDescription& Out) const override
	{
		if (!Super::BuildTouchReflectorDescription(Out)) return false;
		Out.Kind = EWoTTouchReflectorKind::Fork;
		return true;
	}
	virtual bool UsesMottledFeedback() const override { return true; }
	virtual FName ReflectSoundPath() const override { return TEXT("Angreal/ForkFK"); }
};
