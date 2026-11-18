#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

#if !UE_BUILD_SHIPPING
// Development capture aid: -WoTFPScript=<file> runs a timed list of console
// commands once a local player has a pawn, for repeatable first-person frames
// (give an item, pose the hand, HighResShot, quit). Each non-empty line that
// isn't a # comment is "<seconds after the previous line> <console command>".
// Commands run at most one per frame, each timed from the previous one's run,
// so a hitch can't fold an item change and a screenshot into one frame. The
// clock pauses while no local pawn exists, so a respawn delays the rest.
class FWoTFirstPersonCaptureScript
{
public:
	FWoTFirstPersonCaptureScript();
	~FWoTFirstPersonCaptureScript();

private:
	bool Tick(float DeltaTime);

	struct FLine
	{
		double Delay = 0;
		FString Command;
	};
	TArray<FLine> Lines;
	int32 Next = 0;
	double Waited = 0;
	FTSTicker::FDelegateHandle TickHandle;
};
#endif
