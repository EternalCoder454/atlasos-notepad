// The file layer of the Rust core (apps/atlas-notepad/src/lib.rs).
// Text is UTF-16 with "\n" between lines; the file's encoding and line ending
// are kept beside it and given back to np_file_save.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NpFile::encoding
enum {
    NP_UTF8 = 0,
    NP_UTF8_BOM = 1,
    NP_UTF16_LE = 2,
    NP_UTF16_BE = 3,
    NP_WINDOWS_1252 = 4,
};

// NpFile::lineEnding
enum {
    NP_LF = 0,
    NP_CRLF = 1,
    NP_CR = 2,
};

// What a file looked like when read or written. If it differs later, the file
// was changed by someone else.
typedef struct NpStamp {
    int64_t mtimeNs;
    uint64_t size, dev, ino;
} NpStamp;

typedef struct NpFile {
    uint16_t *text;   // len UTF-16 units, "\n" line breaks; null on error
    size_t len;
    uint8_t encoding, lineEnding;
    uint8_t mixed;    // more than one kind of line ending: saving changes some
    uint8_t binary;   // a NUL byte early on: probably not text
    uint8_t lossy;    // malformed UTF-16 was replaced
    NpStamp stamp;
    int32_t error;    // 0, or an errno
} NpFile;

// path: UTF-8, NUL-terminated. Never returns null: on failure error is set
// (EFBIG past maxBytes, EINVAL for anything but a regular file).
NpFile *np_file_read(const char *path, uint64_t maxBytes);
void np_file_free(NpFile *file);

// 0 on success and *stamp is set; > 0 is an errno; -1 means the text can't be
// written in that encoding (*badOffset is the first such UTF-16 offset).
int32_t np_file_save(const char *path, const uint16_t *text, size_t len,
                     uint8_t encoding, uint8_t lineEnding, NpStamp *stamp,
                     size_t *badOffset);

// For files only we may read (the session's): mode 0600, temp file and
// rename, a symlink at path replaced rather than followed. 0 or an errno.
int32_t np_file_save_private(const char *path, const uint8_t *bytes, size_t len);

// 0 and *stamp set, or an errno.
int32_t np_file_stamp(const char *path, NpStamp *stamp);

#ifdef __cplusplus
}
#endif
