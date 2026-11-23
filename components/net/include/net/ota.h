/*
 * Micro OS — an update of a machine over the network (ADR 0022 §7).
 *
 * The image of a machine is a payload and a trailer of a hundred and twenty
 * bytes: the magic, the length, the SHA-256 of the payload and the Ed25519
 * signature of that header (ports/port_x86_64/loader/sign_image.h). The
 * signature is checked **before a byte reaches the slot**, and the update is
 * a state machine with the two slots of the machine:
 *
 *   valid      the slot the machine boots and trusts;
 *   pending    the slot an update wrote. It is the machine of this boot until
 *              the system says it is good (`ota_confirm`); a second boot that
 *              finds it still pending rolls the machine back to the other
 *              slot, which is what the bootloader of the board does with
 *              otadata and what the file form of x86-64 does with its state
 *              record.
 *
 * The transport is the HTTP client of the component: its sink takes the
 * bytes of the file, so a megabyte costs no buffer of a megabyte. What is
 * kept is the digest and the trailer.
 *
 * Two shapes of the same machine live below this header:
 *
 *   components/net/src/ota_file.c   the slots and the state are files of the
 *                                   VFS (/var/ota/slot0.img and a record);
 *                                   the form of the host and of x86-64, where
 *                                   a "reboot" is the second call of
 *                                   ota_boot() in the tests and the smoke;
 *   components/port_esp32/src/ota_slot.c
 *                                   the slots are the ota_0/ota_1 partitions
 *                                   and the state is the otadata of the
 *                                   bootloader, which rolls a boot that never
 *                                   confirms itself back by itself.
 */
#ifndef MICRO_OS_NET_OTA_H
#define MICRO_OS_NET_OTA_H

#include "kernel/types.h"
#include "net/http.h"

/* The largest payload (with its trailer) an update takes: the slot of the
 * board is 1.125 MiB and a machine of another port may have more room. */
#define OTA_IMAGE_MAX (2u * 1024u * 1024u)

/* The directory of the staging file and of the state on a machine whose
 * slots are files. The board has no such directory: its state is otadata. */
#define OTA_STATE_DEFAULT "/var/ota"

/* The trailer of a signed image (ADR 0020), repeated here because the VFS
 * has no include of a port's loader: the two numbers are a contract of the
 * format and a test compares them with sign_image.h. */
#define OTA_TRAILER_SIZE 120
#define OTA_TRAILER_HEADER 56
#define OTA_TRAILER_DIGEST 32
#define OTA_SIGNATURE_SIZE 64
#define OTA_KEY_SIZE 32
#define OTA_KEY_HEX (OTA_KEY_SIZE * 2u + 1u)

/* The states of a slot. NEW is the slot an update wrote and no boot has
 * tried: the boot that finds it makes it PENDING, and the boot after that -
 * a PENDING that was never confirmed - is the rollback. The board's
 * bootloader keeps the same three numbers in otadata (NEW, PENDING_VERIFY,
 * VALID) and rolls back the same way. */
#define OTA_SLOT_VALID 0
#define OTA_SLOT_PENDING 1
#define OTA_SLOT_INVALID 2
#define OTA_SLOT_NEW 3

/* The answers of the calls. OTA_INSTALLED is a success with a reboot after
 * it: the image is in the other slot and the state names it. */
#define OTA_OK 0
#define OTA_INSTALLED 1
#define OTA_EBUSY (-1)   /* another update is running */
#define OTA_EFETCH (-2)  /* the image did not arrive */
#define OTA_ESIZE (-3)   /* it is larger than the slot, or smaller than a trailer */
#define OTA_EFORMAT (-4) /* the trailer is not one of this format */
#define OTA_EDIGEST (-5) /* the bytes are not the ones the trailer hashes */
#define OTA_ESIGN (-6)   /* the trailer is not signed by the key of the machine */
#define OTA_EIO (-7)     /* the volume or the flash refused a call */
#define OTA_ENOKEY (-8)  /* there is no key to check the image with */
#define OTA_ENOMEM (-9)  /* the staging file cannot be made */

struct ota_config {
    const char *url;      /* the signed image, over http or https */
    const char *state;    /* the directory of the file form; NULL is the default */
    const char *key_path; /* a key in hexadecimal, when `key` is NULL */
    const k_u8_t *key;    /* the key of thirty-two bytes, or NULL */
    k_size_t max_image;   /* 0 is OTA_IMAGE_MAX */
    k_u32_t timeout_ms;   /* 0 is the budget of the client */
    const struct http_transport *transport; /* NULL is the sockets of the port */
};

/* What the machine knows about its slots at this boot. */
struct ota_state {
    int active;       /* the slot the machine runs: 0 or 1 */
    int slot_state;   /* OTA_SLOT_VALID, OTA_SLOT_PENDING or OTA_SLOT_INVALID */
    k_u32_t sequence; /* the installs this machine has made */
    bool rolled_back; /* this boot returned to the other slot */
};

/*
 * The boot of the machine, as the state machine sees it. The port reads its
 * state; a slot that is still pending from the boot before did not confirm
 * itself, and the machine returns to the other one. Called once by init or
 * by a script, before anything reads the slots.
 */
int ota_boot(struct ota_state *out);

/* The running slot is good: the next boot keeps it and the rollback is
 * cancelled. A machine that confirms a boot it does not keep is a machine
 * that cannot return; the call is the promise that the system is up. */
int ota_confirm(void);

/* The same read without the rollback: a command of the console. */
int ota_status(struct ota_state *out);

/*
 * Download the image of `url` into the inactive slot, check the trailer and
 * the signature, and make it the next boot. The bytes of the staging file
 * are read back for the digest, so what is verified is what the slot will
 * receive. OTA_INSTALLED means "written and selected": the caller reboots.
 */
int ota_install(const struct ota_config *cfg, struct ota_state *out);

/* The sentence of an answer, and the name of a state. */
const char *ota_strerror(int rc);
const char *ota_state_name(int slot_state);

/*
 * The storage of the slots, which a port implements. The file form of
 * components/net/src/ota_file.c answers for the host and x86-64; the board
 * answers with the partitions of its flash (components/port_esp32).
 *
 * `begin` makes the slot ready to receive the bytes of a new image and
 * forgets what was there; `write` places a block at an offset of the payload
 * (from zero, the trailer follows the payload); `end` flushes what the
 * storage kept; `select` makes the slot the next boot; `invalidate` marks it
 * as one that must not be booted; `size` answers what the slot holds.
 */
int ota_slot_begin(int slot);
int ota_slot_write(int slot, k_u64_t offset, const void *data, k_size_t size);
/*
 * `end` closes the payload and takes the trailer: a slot of the board keeps
 * the trailer in its last 4 KiB, away from the image (ADR 0022), and the
 * file form of x86-64 has nothing to do with it - what boots there is the
 * payload, and the trailer was checked before it.
 */
int ota_slot_end(int slot, const k_u8_t trailer[OTA_TRAILER_SIZE]);
int ota_slot_select(int slot);
int ota_slot_invalidate(int slot);
int ota_slot_size(int slot, k_u64_t *bytes);

#endif /* MICRO_OS_NET_OTA_H */
