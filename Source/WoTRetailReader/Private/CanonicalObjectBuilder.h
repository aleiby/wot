// Checked construction for canonical JSON objects owned by WoTRetailReader.
#pragma once

#include "WoTBootstrapCore/CanonicalJson.h"

/**
 * Canonical objects require strictly ascending keys. AddMember rejects and drops
 * any other key; this builder records that rejection and turns it into an error.
 */
class FWoTCanonicalObjectBuilder final
{
public:
	void Add(const TCHAR* Key, FWoTCanonicalJsonValue&& Value)
	{
		if (!Object.AddMember(Key, MoveTemp(Value)))
		{
			if (bAllAccepted)
			{
				RejectedKey = Key;
			}
			bAllAccepted = false;
		}
	}

	bool IsComplete() const { return bAllAccepted; }

	TWoTBootstrapResult<FWoTCanonicalJsonValue> Build(FStringView Context) &&
	{
		using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
		if (!IsComplete())
		{
			const FString Message = FString(Context)
				+ TEXT(" rejected canonical member '") + RejectedKey
				+ TEXT("'; keys must be strictly ascending");
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::NonCanonicalJson, Message, RejectedKey));
		}
		return FResult::Success(MoveTemp(Object));
	}

private:
	FWoTCanonicalJsonValue Object = FWoTCanonicalJsonValue::MakeObject();
	bool bAllAccepted = true;
	FString RejectedKey;
};
