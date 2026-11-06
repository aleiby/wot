#include "WoTMaterializedWorldHost.h"

#include "Components/SceneComponent.h"

AWoTMaterializedActorHost::AWoTMaterializedActorHost()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("SemanticRoot")));
}

void AWoTMaterializedActorHost::BeginPlay()
{
	++MaterializedBeginPlayCount;
	Super::BeginPlay();
}

AWoTMaterializedWorldRoot::AWoTMaterializedWorldRoot()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("MaterializedRoot")));
}
