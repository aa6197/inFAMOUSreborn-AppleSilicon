/* Disc import (Apple port).
 *
 * Turns the player's own copy of the game into the folder the runtime reads:
 *
 *   <dest_root>/BCES00220/PS3_DISC.SFB
 *   <dest_root>/BCES00220/PS3_GAME/...            (copied from the disc)
 *   <dest_root>/BCES00220/PS3_GAME/USRDIR/EBOOT.elf  (decrypted EBOOT.BIN)
 *
 * The source is a disc image (.iso, decrypted or a 1:1 dump plus its disc key)
 * or a folder dump (a folder holding PS3_GAME). The result is checked against
 * the EBOOT this build was made from, so a different disc or version is
 * refused with a plain message.
 *
 * EBOOT.BIN is a retail SELF. It is decrypted with a key for this one file
 * (eboot_key.h, made by import_cli make-key, not in git). A build without it
 * can still import a folder dump that already has EBOOT.elf.
 *
 * The work happens in <dest_root>/BCES00220.importing and is renamed into
 * place at the end, so a stopped import never looks finished.
 *
 * Plain C, no UI. Used by the iOS shell, the macOS app and import_cli. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DI_TITLE_ID "BCES00220"

typedef enum {
    DI_OK = 0,
    DI_ERR_OPEN,        /* cannot open or read the source */
    DI_ERR_NOT_GAME,    /* not a PS3 disc image or game folder */
    DI_ERR_WRONG_GAME,  /* a PS3 game, but not this one or not this version */
    DI_ERR_NEED_KEY,    /* encrypted 1:1 dump: needs its .dkey */
    DI_ERR_BAD_KEY,     /* the disc key does not fit */
    DI_ERR_SPACE,       /* not enough free space */
    DI_ERR_WRITE,       /* cannot write the game folder */
    DI_ERR_NO_EBOOT_KEY,/* this build cannot decrypt EBOOT.BIN */
    DI_ERR_DECRYPT,     /* EBOOT.BIN did not decrypt */
    DI_ERR_VERIFY,      /* the result is not the expected file */
    DI_ERR_CANCELLED,
} di_status;

typedef struct {
    int      is_iso;          /* 1 = disc image, 0 = folder */
    int      encrypted;       /* 1 = the image needs a disc key */
    int      have_key;        /* a disc key was given and fits */
    char     title_id[16];    /* from PARAM.SFO */
    char     title[128];      /* from PARAM.SFO, UTF-8 */
    char     version[16];     /* PARAM.SFO VERSION */
    uint64_t bytes;           /* what will be written, roughly */
} di_info;

/* Progress, from the worker thread: fraction 0..1 and a short status line. */
typedef void (*di_progress_fn)(void* ctx, double fraction, const char* status);

typedef struct {
    di_progress_fn progress;
    void*          ctx;
    volatile int*  cancel;    /* set to non-zero to stop */
} di_callbacks;

/* Look at a source without writing anything. dkey may be NULL; if it is NULL
 * and the image is encrypted, a <image>.dkey next to it is tried. */
di_status di_probe(const char* src, const char* dkey, di_info* out, char* err, size_t errlen);

/* Import src into dest_root/BCES00220 (see the top of this file). */
di_status di_import(const char* src, const char* dkey, const char* dest_root,
                    const di_callbacks* cb, char* err, size_t errlen);

/* 1 if dest_root/BCES00220 holds a finished import (EBOOT.elf present and
 * the expected file). elf_out (optional) gets its path. */
int di_installed(const char* dest_root, char* elf_out, size_t elf_out_len);

/* 1 if this build carries the key for EBOOT.BIN. */
int di_have_eboot_key(void);

/* Decrypt a SELF to an ELF. meta_key/meta_iv: the file's metadata key and IV
 * (NULL = the key built in). */
di_status di_self_to_elf(const char* self_path, const char* elf_path,
                         const uint8_t* meta_key, const uint8_t* meta_iv,
                         char* err, size_t errlen);

/* Maintainer tool: decrypt the metadata info of a SELF with the APP key
 * (erk 32 bytes, riv 16 bytes) and return its metadata key and IV. */
di_status di_self_meta_key(const char* self_path, const uint8_t erk[32], const uint8_t riv[16],
                           uint8_t meta_key[16], uint8_t meta_iv[16], char* err, size_t errlen);

/* SHA-256 of a file as 64 hex digits. 0 on success. */
int di_sha256_file(const char* path, char hex_out[65]);

const char* di_status_name(di_status s);

#ifdef __cplusplus
}
#endif
