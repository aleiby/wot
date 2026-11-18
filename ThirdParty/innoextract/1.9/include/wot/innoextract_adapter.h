/*
 * WoT callback-I/O adapter for innoextract 1.9.
 *
 * This is the only public surface of the vendored library.  The caller owns the
 * read/write callback contexts; every string and package allocation is owned by
 * the adapter until wot_innoextract_close().  No C++ exception crosses this ABI.
 *
 * Calls into this ABI are serialized by one library-wide mutex because upstream
 * innoextract retains process-global logging/encoding state. Callbacks execute
 * while that mutex is held and MUST NOT re-enter any wot_innoextract_* function.
 * Package pointers may be used from different threads, but calls do not run in
 * parallel; callers must still prevent close from racing another call.
 */
#ifndef WOT_INNOEXTRACT_ADAPTER_H
#define WOT_INNOEXTRACT_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WOT_INNOEXTRACT_ABI_VERSION 1u

typedef struct wot_innoextract_package wot_innoextract_package;

/** Return bytes read, 0 only at EOF, or a negative value on caller I/O error. No re-entry. */
typedef int64_t (*wot_innoextract_read_at_fn)(
    void* context, uint64_t offset, void* output, uint64_t byte_count);

/** Return zero on success and nonzero to abort extraction. No adapter re-entry. */
typedef int32_t (*wot_innoextract_write_fn)(
    void* context, const void* bytes, uint64_t byte_count);

typedef enum wot_innoextract_result {
    WOT_INNOEXTRACT_OK = 0,
    WOT_INNOEXTRACT_INVALID_ARGUMENT = 1,
    WOT_INNOEXTRACT_IO_ERROR = 2,
    WOT_INNOEXTRACT_FORMAT_ERROR = 3,
    WOT_INNOEXTRACT_LIMIT_ERROR = 4,
    WOT_INNOEXTRACT_WRITE_ERROR = 5,
    WOT_INNOEXTRACT_INTERNAL_ERROR = 6
} wot_innoextract_result;

typedef enum wot_innoextract_checksum_type {
    WOT_INNOEXTRACT_CHECKSUM_NONE = 0,
    WOT_INNOEXTRACT_CHECKSUM_ADLER32 = 1,
    WOT_INNOEXTRACT_CHECKSUM_CRC32 = 2,
    WOT_INNOEXTRACT_CHECKSUM_MD5 = 3,
    WOT_INNOEXTRACT_CHECKSUM_SHA1 = 4
} wot_innoextract_checksum_type;

typedef enum wot_innoextract_compression {
    WOT_INNOEXTRACT_COMPRESSION_STORED = 0,
    WOT_INNOEXTRACT_COMPRESSION_ZLIB = 1,
    WOT_INNOEXTRACT_COMPRESSION_BZIP2 = 2,
    WOT_INNOEXTRACT_COMPRESSION_LZMA1 = 3,
    WOT_INNOEXTRACT_COMPRESSION_LZMA2 = 4,
    WOT_INNOEXTRACT_COMPRESSION_UNKNOWN = 255
} wot_innoextract_compression;

typedef struct wot_innoextract_entry {
    const char* name_utf8;
    uint64_t uncompressed_size;
    wot_innoextract_checksum_type checksum_type;
    const char* checksum_hex;
    wot_innoextract_compression compression;
} wot_innoextract_entry;

/** Parse one installer through read_at. The adapter never opens a path or FILE*. */
int32_t wot_innoextract_open(
    wot_innoextract_read_at_fn read_at,
    void* read_context,
    uint64_t input_size,
    wot_innoextract_package** output_package);

uint64_t wot_innoextract_entry_count(const wot_innoextract_package* package);

/** The returned pointers remain valid until wot_innoextract_close(package). */
int32_t wot_innoextract_entry_at(
    const wot_innoextract_package* package,
    uint64_t index,
    wot_innoextract_entry* output_entry);

/** Stream exactly one decoded entry to the caller-owned sink. */
int32_t wot_innoextract_extract_entry(
    const wot_innoextract_package* package,
    uint64_t index,
    wot_innoextract_write_fn write,
    void* write_context);

/** Stable parser description owned by package, used in held-snapshot evidence. */
const char* wot_innoextract_description(const wot_innoextract_package* package);

void wot_innoextract_close(wot_innoextract_package* package);

/** Per-thread diagnostic for the most recent failed call on this thread. */
const char* wot_innoextract_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WOT_INNOEXTRACT_ADAPTER_H */
