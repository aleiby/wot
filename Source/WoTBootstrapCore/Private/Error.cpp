#include "WoTBootstrapCore/Error.h"

DEFINE_LOG_CATEGORY(LogWoTBootstrap);

const TCHAR* LexToString(EWoTBootstrapError Code)
{
	switch (Code)
	{
	case EWoTBootstrapError::InvalidUtf8:         return TEXT("InvalidUtf8");
	case EWoTBootstrapError::DuplicateField:      return TEXT("DuplicateField");
	case EWoTBootstrapError::UnknownField:        return TEXT("UnknownField");
	case EWoTBootstrapError::InvalidNumber:       return TEXT("InvalidNumber");
	case EWoTBootstrapError::NonCanonicalJson:    return TEXT("NonCanonicalJson");
	case EWoTBootstrapError::SizeLimit:           return TEXT("SizeLimit");
	case EWoTBootstrapError::UnsupportedProtocol: return TEXT("UnsupportedProtocol");
	case EWoTBootstrapError::Io:                  return TEXT("Io");
	case EWoTBootstrapError::Crypto:              return TEXT("Crypto");
	case EWoTBootstrapError::Rollback:            return TEXT("Rollback");
	case EWoTBootstrapError::UnsafePath:          return TEXT("UnsafePath");
	case EWoTBootstrapError::Durability:          return TEXT("Durability");
	case EWoTBootstrapError::ServiceUnavailable:  return TEXT("ServiceUnavailable");
	case EWoTBootstrapError::CorruptInstallation: return TEXT("CorruptInstallation");
	}

	// No default label above: adding a code without a name is a compile-time gap
	// on both toolchains, and this line only guards a value cast in from outside
	// the enum.
	return TEXT("UnknownErrorCode");
}

FString FWoTBootstrapError::ToString() const
{
	if (Path.IsEmpty())
	{
		return FString::Printf(TEXT("%s: %s"), LexToString(Code), *Message);
	}
	return FString::Printf(TEXT("%s: %s (%s)"), LexToString(Code), *Message, *Path);
}
