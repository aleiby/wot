#include "WoTFirstPersonCaptureScript.h"

#if !UE_BUILD_SHIPPING
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTFirstPersonScript, Log, All);

namespace
{
APlayerController* LocalPlayerWithPawn()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (!World || !World->IsGameWorld()) continue;
		APlayerController* Controller = World->GetFirstPlayerController();
		if (Controller && Controller->IsLocalController() && Controller->GetPawn()) return Controller;
	}
	return nullptr;
}
} // namespace

FWoTFirstPersonCaptureScript::FWoTFirstPersonCaptureScript()
{
	FString Path;
	if (!FParse::Value(FCommandLine::Get(), TEXT("WoTFPScript="), Path)) return;
	TArray<FString> Raw;
	if (!FFileHelper::LoadFileToStringArray(Raw, *Path))
	{
		UE_LOG(LogWoTFirstPersonScript, Error, TEXT("cannot read %s"), *Path);
		return;
	}
	for (const FString& RawLine : Raw)
	{
		const FString Line = RawLine.TrimStartAndEnd();
		FString Delay, Command;
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || !Line.Split(TEXT(" "), &Delay, &Command)) continue;
		Lines.Add({FMath::Max(FCString::Atod(*Delay), 0.0), Command.TrimStartAndEnd()});
	}
	UE_LOG(LogWoTFirstPersonScript, Display, TEXT("%d commands from %s"), Lines.Num(), *Path);
	if (Lines.Num() > 0)
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FWoTFirstPersonCaptureScript::Tick));
	}
}

FWoTFirstPersonCaptureScript::~FWoTFirstPersonCaptureScript()
{
	if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
}

bool FWoTFirstPersonCaptureScript::Tick(float DeltaTime)
{
	APlayerController* Controller = LocalPlayerWithPawn();
	if (!Controller) return true;
	Waited += DeltaTime;
	if (Waited >= Lines[Next].Delay)
	{
		UE_LOG(LogWoTFirstPersonScript, Display, TEXT("%d/%d after %.2fs: %s"), Next + 1, Lines.Num(), Waited, *Lines[Next].Command);
		Controller->ConsoleCommand(Lines[Next].Command, true);
		Waited = 0;
		++Next;
	}
	if (Next >= Lines.Num()) TickHandle.Reset();
	return Next < Lines.Num();
}
#endif
