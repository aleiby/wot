#include "WoTVertexAnimTestActor.h"

AWoTVertexAnimTestActor::AWoTVertexAnimTestActor()
{
	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
}

void AWoTVertexAnimTestActor::BeginPlay()
{
	Super::BeginPlay();
	if (!PlaySequence.IsNone())
	{
		Mesh->LoopAnim(PlaySequence, PlayRate);
	}
}
