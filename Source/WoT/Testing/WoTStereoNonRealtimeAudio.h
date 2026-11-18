#pragma once

#include "CoreMinimal.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

// The trailer rig's audio device. The engine's -DeterministicAudio renderer
// mixes two channels but lists a 7.1 speaker layout, so the 3D panner builds
// eight gains for a two-channel output and the source output zeroes the
// mismatched map (AudioMixerSourceOutputBuffer.cpp SetChannelMap): every
// spatialized sound plays silent. Launching with
// -ini:Engine:[Audio]:AudioMixerModuleName=WoTStereoNonRealtime selects the same
// renderer with its speaker list cut to the channels it mixes.
namespace WoTStereoNonRealtimeAudio
{
	// Registers the WoTStereoNonRealtime audio device module. Call before the
	// engine creates its audio device (the game module's startup is early enough).
	void Register();
}

#endif
