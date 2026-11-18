// Domain-separated Ed25519 verification.
//
// This module VERIFIES ONLY. There is no signing entry point and no private-key
// type anywhere in it: signing happens in an offline ceremony, and a shipped
// binary that could sign would defeat the point of an offline root.
//
// Every signature covers a framed input rather than the artifact bytes directly:
//
//   "WOT-SIGNATURE\0" || uint16le(context) || uint64le(canonical_size) || canonical_bytes
//
// The context byte is why a valid announcement signature cannot be replayed as a
// release-manifest signature, and the explicit size is why two artifacts cannot be
// concatenated into one signed input. Both integers are little-endian; that is
// part of the signed bytes, so the Python release signer must frame identically.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

/** The 24-byte framing prefix: 14 bytes of tag, 2 of context, 8 of size. */
inline constexpr int32 WoTSignatureFramePrefixBytes = 24;

WOTBOOTSTRAPCORE_API TArray<uint8> FrameSignatureInput(
	EWoTSignatureContext Context, TConstArrayView<uint8> CanonicalBytes);

/**
 * Verify a detached signature over the framed artifact bytes.
 *
 * Success(true) means the signature is valid for this key AND this domain.
 * Success(false) means it is not — an ordinary, expected answer. An Error means the
 * question could not be asked at all (unusable key bytes, OpenSSL failure), which
 * callers must never conflate with a clean rejection.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool> VerifyEd25519(
	const FWoTEd25519PublicKey& Key, EWoTSignatureContext Context,
	TConstArrayView<uint8> CanonicalBytes, const FWoTEd25519Signature& Signature);

/**
 * Verify a signature over raw, UNFRAMED bytes.
 *
 * This exists for exactly one reason: RFC 8032's published vectors sign a bare
 * message, and framing them would make them untestable. Production code must
 * never call it — a signature checked without a domain can be replayed into
 * another domain, which is the attack the framing above exists to stop. The name
 * is deliberately awkward so that a call site is obvious in review.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool> WoTVerifyEd25519RawForTesting(
	const FWoTEd25519PublicKey& Key, TConstArrayView<uint8> Message,
	const FWoTEd25519Signature& Signature);
