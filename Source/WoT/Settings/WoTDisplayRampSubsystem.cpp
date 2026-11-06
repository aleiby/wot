#include "WoTDisplayRampSubsystem.h"

#include "Components/SceneCaptureComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "UObject/UObjectIterator.h"

namespace
{
const TCHAR* DisplayCollectionPath =
	TEXT("/Game/WoT/Materials/MPC_WoTDisplay.MPC_WoTDisplay");
const FName RampParameterName(TEXT("RampInvGamma"));
float CurrentRampInvGamma = 1.0f / 1.4f;
uint64 RampRevision = 1;
}

bool UWoTDisplayRampSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer)
		&& World && World->IsGameWorld()
		&& !GIsEditor && !IsRunningCommandlet() && !IsRunningDedicatedServer()
		&& World->GetNetMode() != NM_DedicatedServer;
}

void UWoTDisplayRampSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Refresh();
}

void UWoTDisplayRampSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	Refresh();
}

void UWoTDisplayRampSubsystem::Tick(float)
{
	if (AppliedRevision != RampRevision)
	{
		Refresh();
	}
}

TStatId UWoTDisplayRampSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWoTDisplayRampSubsystem, STATGROUP_Tickables);
}

void UWoTDisplayRampSubsystem::SetRampInvGamma(const float RampInvGamma)
{
	CurrentRampInvGamma = RampInvGamma;
	++RampRevision;
	if (!GEngine)
	{
		return;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (UWorld* World = Context.World())
		{
			if (UWoTDisplayRampSubsystem* Subsystem =
				World->GetSubsystem<UWoTDisplayRampSubsystem>())
			{
				Subsystem->Refresh();
			}
		}
	}
}

void UWoTDisplayRampSubsystem::Refresh()
{
	if (IsRenderWorld() && ApplyToWorld(GetWorld(), CurrentRampInvGamma))
	{
		AppliedRevision = RampRevision;
	}
}

bool UWoTDisplayRampSubsystem::IsRenderWorld() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (World->GetGameViewport())
	{
		return true;
	}
	for (TObjectIterator<USceneCaptureComponent> It; It; ++It)
	{
		if (!It->HasAnyFlags(RF_ClassDefaultObject) && It->GetWorld() == World)
		{
			return true;
		}
	}
	return false;
}

bool UWoTDisplayRampSubsystem::ApplyToWorld(
	UWorld* World, const float RampInvGamma)
{
	if (!World)
	{
		return false;
	}
	UMaterialParameterCollection* Collection =
		LoadObject<UMaterialParameterCollection>(nullptr, DisplayCollectionPath);
	if (!Collection)
	{
		return false;
	}
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, RampParameterName, RampInvGamma);
	return true;
}

#if WITH_AUTOMATION_TESTS
bool UWoTDisplayRampSubsystem::ApplyToWorldForTesting(
	UWorld* World, const float RampInvGamma)
{
	return ApplyToWorld(World, RampInvGamma);
}
#endif
