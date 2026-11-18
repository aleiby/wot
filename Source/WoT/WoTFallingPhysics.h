#pragma once

class AActor;
class UPrimitiveComponent;

namespace WoTFallingPhysics
{
	// The original engine's default zone terminal speed, in units per second.
	// It bounds the whole velocity vector of a non-bouncing falling actor.
	inline constexpr float TerminalSpeed = 2500.0f;

	// The original's falling physics begins every step by checking that the
	// actor is still inside the level (UWoTZoneIndex::IsPointOutsideWorld). Call
	// this at the start of a falling step: an actor outside the level falls out
	// of the world (AActor::FellOutOfWorld). Anything but a pawn stops and is
	// destroyed on the server; a pawn dies (AWoTCharacter::FellOutOfWorld).
	// Returns true when the actor is outside the level, and the step must not
	// run. Worlds without the map's zone tree cannot establish that it has left.
	bool DestroyOutsideWorld(AActor& Actor);

	// If Shape begins a falling step inside a floor, moves its actor up onto the
	// floor's top, found by sweeping the shape down from just above its current
	// position. The physics engine's own push-out direction is not used: for a
	// shape centred over a box it differs by platform and can point down,
	// through the floor. Returns true when the actor was moved.
	bool LiftOutOfFloor(UPrimitiveComponent& Shape);
}
