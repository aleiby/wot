#include "WoTDoorTrigger.h"

void AWoTDoorTrigger::FireTargetEvent(AActor* Other, APawn* EventInstigator)
{
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireDoorEvent(
		EventName, Direction == EWoTDoorDirection::Forward, Other, EventInstigator);
}
