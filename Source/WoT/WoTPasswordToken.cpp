#include "WoTPasswordToken.h"

#include "WoTSha256.h"

FString WoTPasswordToken::Derive(const FString& Password)
{
	const FString HashInput =
		TEXT("WoT.Reborn.JoinPassword.v1:") + Password.ToUpper();
	const FTCHARToUTF8 Utf8(*HashInput);
	return WoTSha256::HashBuffer(Utf8.Get(), Utf8.Length());
}
