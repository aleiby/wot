// The strict canonical JSON grammar shared by every signed bootstrap artifact.
//
// "Canonical" here is exact, not approximate: for any accepted document,
// WriteCanonicalJson(ParseCanonicalJson(Bytes)) reproduces Bytes byte for byte.
// That is what makes a detached signature over these bytes meaningful — two
// implementations cannot disagree about what was signed. The form is defined to
// match `json.dumps(value, sort_keys=True, separators=(",", ":"),
// ensure_ascii=True)`, which the Python release tooling already emits:
//
//   * no whitespace anywhere, minimal separators (`,` and `:`)
//   * object keys strictly increasing by codepoint; duplicates rejected
//   * integers only — no fraction, no exponent, no -0, no leading zeros
//   * ASCII-only output; every other codepoint appears as a lowercase \uXXXX
//     escape (surrogate pair above the BMP), and the shorthand escapes
//     \b \f \n \r \t are mandatory where they exist
//
// Anything else is rejected rather than normalized. A parser that repaired its
// input would let two callers sign different bytes for the same document.
//
// This header is the sole complete definition owner of FWoTCanonicalJsonValue.
// No artifact codec declares a private JSON value variant.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "WoTBootstrapCore/Error.h"

/** The closed value kind set. Notably absent: any floating-point kind. */
enum class EWoTCanonicalJsonKind : uint8
{
	Null,
	Bool,
	Integer,
	String,
	Array,
	Object,
};

/**
 * Hostile-input bounds, applied while parsing rather than after.
 *
 * Defaults are sized for signed artifacts (manifests, key rings, permits), not
 * for arbitrary documents; a caller that needs different bounds passes them
 * explicitly so the limit is visible at the call site.
 */
struct FWoTCanonicalJsonLimits
{
	uint32 MaxBytes = 4 * 1024 * 1024;

	/** Nesting depth measured in containers. Exactly MaxDepth is allowed. */
	uint16 MaxDepth = 32;

	/** Members per object and elements per array. */
	uint32 MaxMembers = 65536;

	/** Decoded UTF-8 length of one string. */
	uint32 MaxStringBytes = 1024 * 1024;
};

/**
 * One canonical JSON value.
 *
 * Move-only and self-owning. Object members are stored in parsed order, which is
 * also sorted order — the parser rejects any key that is not strictly greater
 * than its predecessor, so position and ordering carry the same information and
 * a value read from bytes can always be written back unchanged.
 */
class WOTBOOTSTRAPCORE_API FWoTCanonicalJsonValue final
{
public:
	FWoTCanonicalJsonValue();
	~FWoTCanonicalJsonValue();

	FWoTCanonicalJsonValue(FWoTCanonicalJsonValue&& Other);
	FWoTCanonicalJsonValue& operator=(FWoTCanonicalJsonValue&& Other);

	FWoTCanonicalJsonValue(const FWoTCanonicalJsonValue&) = delete;
	FWoTCanonicalJsonValue& operator=(const FWoTCanonicalJsonValue&) = delete;

	static FWoTCanonicalJsonValue MakeNull();
	static FWoTCanonicalJsonValue MakeBool(bool bValue);
	static FWoTCanonicalJsonValue MakeInteger(int64 Value);
	static FWoTCanonicalJsonValue MakeString(FString Value);
	static FWoTCanonicalJsonValue MakeArray();
	static FWoTCanonicalJsonValue MakeObject();

	EWoTCanonicalJsonKind GetKind() const { return Kind; }
	bool IsNull() const { return Kind == EWoTCanonicalJsonKind::Null; }
	bool IsArray() const { return Kind == EWoTCanonicalJsonKind::Array; }
	bool IsObject() const { return Kind == EWoTCanonicalJsonKind::Object; }

	bool GetBool() const;
	int64 GetInteger() const;
	const FString& GetString() const;

	/** Element count for an array, member count for an object. */
	int32 Num() const;

	const FWoTCanonicalJsonValue& GetElement(int32 Index) const;
	const FString& GetMemberKey(int32 Index) const;
	const FWoTCanonicalJsonValue& GetMemberValue(int32 Index) const;

	/** Ordered lookup; null when absent. Never creates a member. */
	const FWoTCanonicalJsonValue* FindMember(FStringView Key) const;

	void AddElement(FWoTCanonicalJsonValue&& Element);

	/**
	 * Append one object member.
	 *
	 * Returns false — and adds nothing — when Key is not strictly greater than
	 * the last key, so a hand-built value cannot serialize to non-canonical
	 * bytes. Callers that must distinguish "duplicate" from "out of order"
	 * compare the keys themselves; the parser does exactly that to choose
	 * between DuplicateField and NonCanonicalJson.
	 */
	bool AddMember(FString Key, FWoTCanonicalJsonValue&& Value);

private:
	EWoTCanonicalJsonKind Kind = EWoTCanonicalJsonKind::Null;
	bool bBoolValue = false;
	int64 IntegerValue = 0;
	FString StringValue;

	/** Object member keys, in canonical order. Empty for every other kind. */
	TArray<FString> MemberKeys;

	/**
	 * Array elements, or the values paired positionally with MemberKeys. Held
	 * behind unique pointers so this class can contain itself; the destructor and
	 * move operations are defined out of line for the same reason.
	 */
	TArray<TUniquePtr<FWoTCanonicalJsonValue>> Children;
};

/**
 * Parse exactly the canonical grammar, or fail.
 *
 * Errors are specific: InvalidUtf8 for malformed or surrogate-invalid text,
 * DuplicateField for a repeated key, InvalidNumber for a non-integer or
 * out-of-int64 number, SizeLimit for a bound in Limits, and NonCanonicalJson for
 * text that is valid JSON but not the canonical spelling of it.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTCanonicalJsonValue> ParseCanonicalJson(
	TConstArrayView<uint8> Bytes, const FWoTCanonicalJsonLimits& Limits = FWoTCanonicalJsonLimits());

/**
 * Serialize to canonical bytes, or fail.
 *
 * Fails rather than emitting non-canonical output: an object whose members are
 * out of order, or a string carrying an unpaired surrogate, has no canonical
 * form and must not be signed.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<TArray<uint8>> WriteCanonicalJson(
	const FWoTCanonicalJsonValue& Value);

// --- Closed-schema readers ---------------------------------------------------
//
// Every signed artifact in this module has a CLOSED schema, and these are the only
// accessors used to read one. They exist here rather than once per codec so that
// the rule is enforced identically everywhere: a field set must match exactly, a
// field must have the expected type, and nothing is defaulted.
//
// All of them report UnknownField for a schema mismatch — unknown, missing, or
// wrong-typed — because the closed error set has one code for "this document is
// not the shape I was written against". The message says which way it failed.

/**
 * Require an object's member set to be exactly `Expected`.
 *
 * Both directions. An unknown field is refused because it may be the one carrying
 * a restriction this build does not implement; a missing field is refused because
 * supplying a default would be a policy decision the artifact did not make.
 */
WOTBOOTSTRAPCORE_API TOptional<FWoTBootstrapError> RequireExactMembers(
	const FWoTCanonicalJsonValue& Object, TConstArrayView<const TCHAR*> Expected,
	const TCHAR* What);

WOTBOOTSTRAPCORE_API TOptional<FWoTBootstrapError> ReadStringField(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const TCHAR* What,
	FString& OutValue);

WOTBOOTSTRAPCORE_API TOptional<FWoTBootstrapError> ReadBoolField(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const TCHAR* What,
	bool& bOutValue);

/** Reads a non-negative integer. A negative value is InvalidNumber, not a small one. */
WOTBOOTSTRAPCORE_API TOptional<FWoTBootstrapError> ReadUnsignedField(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const TCHAR* What,
	uint64& OutValue);

/** As above, additionally bounded to uint32 — protocol numbers are not 64-bit. */
WOTBOOTSTRAPCORE_API TOptional<FWoTBootstrapError> ReadUnsigned32Field(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const TCHAR* What,
	uint32& OutValue);

WOTBOOTSTRAPCORE_API TOptional<FWoTBootstrapError> ReadObjectField(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const TCHAR* What,
	const FWoTCanonicalJsonValue*& OutValue);
