//
// test_node_content_ota_apply.c - the OTA consumer, on a host, with no flash anywhere near it.
//
// THIS IS THE SUITE THE SEAMS EXIST FOR. The module's job is to decide things -- which bundles to
// take, what to do when one will not write, how many times to try before a bundle is poison -- and
// every one of those decisions is reachable here because the slot and the hash are vtables the test
// supplies. The scripted slot can be told to fail at begin, at write, at end or at activate, which
// is four failure paths that on a device would each need a deliberately broken bundle and a cable.
//
// THE HASH IS SCRIPTED RATHER THAN REAL, and that is the stronger test: what this module owes its
// caller is that it compares the payload digest against the header and refuses a mismatch, and
// proving that needs a digest the test controls, not a real sha256 it would have to collide.
//
// The ATTEMPT COUNTER is the reason the module exists in this shape, so most of the second half is
// about it: that it is written down BEFORE the slot is activated, that it survives into a new
// process, that the three-way "did it boot" answer is right in each case, and that a bundle which
// has used up its attempts is refused rather than retried for ever.
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

/* iotdata.c for the kvr reader the OTA header parses with, and the variant it needs to compile --
   the same pair test_node_content_ota.c takes, and for the same reason. */
#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"

/* A SMALL CHUNK ON PURPOSE. The real one is 4 KB, which would move any payload this suite can
   afford to build in a single call -- and the whole point of work() being ticked is that it does
   NOT. At 256 a 1500-byte image takes six passes, so the resumption between them is under test
   rather than assumed. */
#define IOTDATA_NODE_OTA_APPLY_CHUNK 256u

/* CRC-64 LIVES IN d_common.h, which needs a platform under it for its timer and delay helpers --
   so the host platform goes in first, exactly as the gateway does it. This suite calls neither;
   it is here for the checksum iotdata_node_content.h sums an upload with. */
#include "d_platform_linux.h"
#include "d_common.h"
#include "d_module_filestore.h" /* the seam: content.h is written against it */
#include "iotdata_node_content.h"
#include "d_module_otaslot.h"
#include "iotdata_node_content_ota.h"
#include "d_module_filestore_ram.h"
#include "d_module_inflate_linux.h" /* the REAL raw-deflate codec, not a stand-in: see its header */
#include "d_module_hpatch.h"        /* and the REAL patcher, which needs no platform at all */
/* and its ENGINE, which lives in a .c. This suite drives the apply path directly, without the
   facade in iotdata_node_otaupdate.h that compiles HPatchLite in for an application, so it
   compiles it in itself -- the same unity-build move as iotdata.c above. */
#include "hpatch_lite.c"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define R_MINE   IOTDATA_NODE_CONTENT_R_MINE
#define R_DELETE IOTDATA_NODE_CONTENT_R_DELETE
#define R_WORK   IOTDATA_NODE_CONTENT_R_WORK
#define R_REBOOT IOTDATA_NODE_CONTENT_R_REBOOT

/* ------------------------------------------------------------------------------------------- */
/* THE SCRIPTED SLOT. Counts what it was asked and refuses whatever the test told it to.       */

typedef struct {
    uint8_t image[4096];
    uint32_t len, declared;
    bool open, ended, active;
    int fail_begin, fail_write, fail_end, fail_activate; /* >0: refuse the next N calls */
    int n_begin, n_write, n_end, n_activate, n_abort;
} fake_slot_t;

/* ------------------------------------------------------------------------------------------- */
/* A REAL DELTA, cut by the same hdiffi the bundler calls, embedded so the suite needs no tools.  */
/*                                                                                               */
/* Produced by:                                                                                  */
/*   hdiffi -f -d base.bin new.bin patch.bin        (uncompressed: FORM and CODEC are separate)   */
/* where base is 900 bytes of long copyable runs, and new is that with a ten-byte edit, a         */
/* twenty-byte string replaced and forty bytes of growth -- which is roughly what a rebuild does. */
/* Regenerate with the same command if the vectors ever need to change; nothing here is derived.  */

static const uint8_t DELTA_BASE[900] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x05, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16,
    0x16, 0x17, 0x17, 0x17, 0x17, 0x17, 0x18, 0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19, 0x19, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21, 0x21, 0x21, 0x21, 0x21, 0x22, 0x22, 0x22, 0x22, 0x22, 0x23, 0x23, 0x23, 0x23, 0x23, 0x24, 0x24, 0x24, 0x24, 0x24, 0x25, 0x25, 0x25, 0x25, 0x25,
    0x26, 0x26, 0x26, 0x26, 0x26, 0x27, 0x27, 0x27, 0x27, 0x27, 0x28, 0x28, 0x28, 0x28, 0x28, 0x29, 0x29, 0x29, 0x29, 0x29, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2D, 0x2D, 0x2D,
    0x2D, 0x2D, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2F, 0x2F, 0x2F, 0x2F, 0x2F, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x31, 0x31, 0x31, 0x31, 0x32, 0x32, 0x32, 0x32, 0x32, 0x33, 0x33, 0x33, 0x33, 0x33, 0x34, 0x34, 0x34, 0x34, 0x34, 0x35,
    0x35, 0x35, 0x35, 0x35, 0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x37, 0x37, 0x37, 0x37, 0x38, 0x38, 0x38, 0x38, 0x38, 0x39, 0x39, 0x39, 0x39, 0x39, 0x3A, 0x3A, 0x3A, 0x3A, 0x3A, 0x3B, 0x3B, 0x3B, 0x3B, 0x3B, 0x3C, 0x3C, 0x3C, 0x3C,
    0x3C, 0x3D, 0x3D, 0x3D, 0x3D, 0x3D, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x40, 0x40, 0x40, 0x40, 0x40, 0x41, 0x41, 0x41, 0x41, 0x41, 0x42, 0x42, 0x42, 0x42, 0x42, 0x43, 0x43, 0x43, 0x43, 0x43, 0x44, 0x44,
    0x44, 0x44, 0x44, 0x45, 0x45, 0x45, 0x45, 0x45, 0x46, 0x46, 0x46, 0x46, 0x46, 0x47, 0x47, 0x47, 0x47, 0x47, 0x48, 0x48, 0x48, 0x48, 0x48, 0x49, 0x49, 0x49, 0x49, 0x49, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4B, 0x4B, 0x4B, 0x4B, 0x4B,
    0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4F, 0x4F, 0x4F, 0x4F, 0x4F, 0x50, 0x50, 0x50, 0x50, 0x50, 0x51, 0x51, 0x51, 0x51, 0x51, 0x52, 0x52, 0x52, 0x52, 0x52, 0x53, 0x53, 0x53,
    0x53, 0x53, 0x54, 0x54, 0x54, 0x54, 0x54, 0x55, 0x55, 0x55, 0x55, 0x55, 0x56, 0x56, 0x56, 0x56, 0x56, 0x57, 0x57, 0x57, 0x57, 0x57, 0x58, 0x58, 0x58, 0x58, 0x58, 0x59, 0x59, 0x59, 0x59, 0x59, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5B,
    0x5B, 0x5B, 0x5B, 0x5B, 0x5C, 0x5C, 0x5C, 0x5C, 0x5C, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5E, 0x5E, 0x5E, 0x5E, 0x5E, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x60, 0x60, 0x60, 0x60, 0x60, 0x61, 0x61, 0x61, 0x61, 0x61, 0x62, 0x62, 0x62, 0x62,
    0x62, 0x63, 0x63, 0x63, 0x63, 0x63, 0x64, 0x64, 0x64, 0x64, 0x64, 0x65, 0x65, 0x65, 0x65, 0x65, 0x66, 0x66, 0x66, 0x66, 0x66, 0x67, 0x67, 0x67, 0x67, 0x67, 0x68, 0x68, 0x68, 0x68, 0x68, 0x69, 0x69, 0x69, 0x69, 0x69, 0x6A, 0x6A,
    0x6A, 0x6A, 0x6A, 0x6B, 0x6B, 0x6B, 0x6B, 0x6B, 0x6C, 0x6C, 0x6C, 0x6C, 0x6C, 0x6D, 0x6D, 0x6D, 0x6D, 0x6D, 0x6E, 0x6E, 0x6E, 0x6E, 0x6E, 0x6F, 0x6F, 0x6F, 0x6F, 0x6F, 0x70, 0x70, 0x70, 0x70, 0x70, 0x71, 0x71, 0x71, 0x71, 0x71,
    0x72, 0x72, 0x72, 0x72, 0x72, 0x73, 0x73, 0x73, 0x73, 0x73, 0x74, 0x74, 0x74, 0x74, 0x74, 0x75, 0x75, 0x75, 0x75, 0x75, 0x76, 0x76, 0x76, 0x76, 0x76, 0x77, 0x77, 0x77, 0x77, 0x77, 0x78, 0x78, 0x78, 0x78, 0x78, 0x79, 0x79, 0x79,
    0x79, 0x79, 0x7A, 0x7A, 0x7A, 0x7A, 0x7A, 0x7B, 0x7B, 0x7B, 0x7B, 0x7B, 0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x7D, 0x7D, 0x7D, 0x7D, 0x7D, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x82, 0x82, 0x82, 0x82, 0x82, 0x83, 0x83, 0x83, 0x83, 0x83, 0x84, 0x84, 0x84, 0x84, 0x84, 0x85, 0x85, 0x85, 0x85, 0x85, 0x86, 0x86, 0x86, 0x86, 0x86, 0x87, 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88, 0x88,
    0x88, 0x89, 0x89, 0x89, 0x89, 0x89, 0x8A, 0x8A, 0x8A, 0x8A, 0x8A, 0x8B, 0x8B, 0x8B, 0x8B, 0x8B, 0x8C, 0x8C, 0x8C, 0x8C, 0x8C, 0x8D, 0x8D, 0x8D, 0x8D, 0x8D, 0x8E, 0x8E, 0x8E, 0x8E, 0x8E, 0x8F, 0x8F, 0x8F, 0x8F, 0x8F, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x91, 0x91, 0x91, 0x91, 0x91, 0x92, 0x92, 0x92, 0x92, 0x92, 0x93, 0x93, 0x93, 0x93, 0x93, 0x94, 0x94, 0x94, 0x94, 0x94, 0x95, 0x95, 0x95, 0x95, 0x95, 0x96, 0x96, 0x96, 0x96, 0x96, 0x97, 0x97, 0x97, 0x97, 0x97,
    0x98, 0x98, 0x98, 0x98, 0x98, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9A, 0x9A, 0x9A, 0x9A, 0x9A, 0x9B, 0x9B, 0x9B, 0x9B, 0x9B, 0x9C, 0x9C, 0x9C, 0x9C, 0x9C, 0x9D, 0x9D, 0x9D, 0x9D, 0x9D, 0x9E, 0x9E, 0x9E, 0x9E, 0x9E, 0x9F, 0x9F, 0x9F,
    0x9F, 0x9F, 0xA0, 0xA0, 0xA0, 0xA0, 0xA0, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA2, 0xA2, 0xA2, 0xA2, 0xA2, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA7,
    0xA7, 0xA7, 0xA7, 0xA7, 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, 0xA9, 0xA9, 0xA9, 0xA9, 0xA9, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAC, 0xAC, 0xAC, 0xAC, 0xAC, 0xAD, 0xAD, 0xAD, 0xAD, 0xAD, 0xAE, 0xAE, 0xAE, 0xAE,
    0xAE, 0xAF, 0xAF, 0xAF, 0xAF, 0xAF, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB1, 0xB1, 0xB1, 0xB1, 0xB1, 0xB2, 0xB2, 0xB2, 0xB2, 0xB2, 0xB3, 0xB3, 0xB3, 0xB3, 0xB3,
};

static const uint8_t DELTA_NEW[940] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x05, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13, 0x13, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x16, 0x16, 0x16, 0x16,
    0x16, 0x17, 0x17, 0x17, 0x17, 0x17, 0x18, 0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19, 0x19, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21, 0x21, 0x21, 0x21, 0x21, 0x22, 0x22, 0x22, 0x22, 0x22, 0x23, 0x23, 0x23, 0x23, 0x23, 0x24, 0x24, 0x24, 0x24, 0x24, 0x25, 0x25, 0x25, 0x25, 0x25,
    0x26, 0x26, 0x26, 0x26, 0x26, 0x27, 0x27, 0x27, 0x27, 0x27, 0x28, 0x28, 0x28, 0x28, 0x28, 0x29, 0x29, 0x29, 0x29, 0x29, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2D, 0x2D, 0x2D,
    0x2D, 0x2D, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2F, 0x2F, 0x2F, 0x2F, 0x2F, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x31, 0x31, 0x31, 0x31, 0x32, 0x32, 0x32, 0x32, 0x32, 0x33, 0x33, 0x33, 0x33, 0x33, 0x34, 0x34, 0x34, 0x34, 0x34, 0x35,
    0x35, 0x35, 0x35, 0x35, 0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x37, 0x37, 0x37, 0x37, 0x38, 0x38, 0x38, 0x38, 0x38, 0x39, 0x39, 0x39, 0x39, 0x39, 0x3A, 0x3A, 0x3A, 0x3A, 0x3A, 0x3B, 0x3B, 0x3B, 0x3B, 0x3B, 0x3C, 0x3C, 0x3C, 0x3C,
    0x3C, 0x3D, 0x3D, 0x3D, 0x3D, 0x3D, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x40, 0x40, 0x40, 0x40, 0x40, 0x41, 0x41, 0x41, 0x41, 0x41, 0x42, 0x42, 0x42, 0x42, 0x42, 0x43, 0x43, 0x43, 0x43, 0x43, 0x44, 0x44,
    0x44, 0x44, 0x44, 0x45, 0x45, 0x45, 0x45, 0x45, 0x46, 0x46, 0x46, 0x46, 0x46, 0x47, 0x47, 0x47, 0x47, 0x47, 0x48, 0x48, 0x48, 0x48, 0x48, 0x49, 0x49, 0x49, 0x49, 0x49, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4B, 0x4B, 0x4B, 0x4B, 0x4B,
    0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4F, 0x4F, 0x4F, 0x4F, 0x4F, 0x50, 0x50, 0x50, 0x50, 0x50, 0x51, 0x51, 0x51, 0x51, 0x51, 0x52, 0x52, 0x52, 0x52, 0x52, 0x53, 0x53, 0x53,
    0x53, 0x53, 0x54, 0x54, 0x54, 0x54, 0x54, 0x55, 0x55, 0x55, 0x55, 0x55, 0x56, 0x56, 0x56, 0x56, 0x56, 0x57, 0x57, 0x57, 0x57, 0x57, 0x58, 0x58, 0x58, 0x58, 0x58, 0x59, 0x59, 0x59, 0x59, 0x59, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5B,
    0x5B, 0x5B, 0x5B, 0x5B, 0x5C, 0x5C, 0x5C, 0x5C, 0x5C, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5E, 0x5E, 0x5E, 0x5E, 0x5E, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x60, 0x60, 0x60, 0x60, 0x60, 0x61, 0x61, 0x61, 0x61, 0x61, 0x62, 0x62, 0x62, 0x62,
    0x62, 0x63, 0x63, 0x63, 0x63, 0x63, 0x61, 0x20, 0x6E, 0x65, 0x77, 0x20, 0x62, 0x75, 0x69, 0x6C, 0x64, 0x20, 0x73, 0x74, 0x61, 0x6D, 0x70, 0x21, 0x21, 0x21, 0x68, 0x68, 0x68, 0x68, 0x68, 0x69, 0x69, 0x69, 0x69, 0x69, 0x6A, 0x6A,
    0x6A, 0x6A, 0x6A, 0x6B, 0x6B, 0x6B, 0x6B, 0x6B, 0x6C, 0x6C, 0x6C, 0x6C, 0x6C, 0x6D, 0x6D, 0x6D, 0x6D, 0x6D, 0x6E, 0x6E, 0x6E, 0x6E, 0x6E, 0x6F, 0x6F, 0x6F, 0x6F, 0x6F, 0x70, 0x70, 0x70, 0x70, 0x70, 0x71, 0x71, 0x71, 0x71, 0x71,
    0x72, 0x72, 0x72, 0x72, 0x72, 0x73, 0x73, 0x73, 0x73, 0x73, 0x74, 0x74, 0x74, 0x74, 0x74, 0x75, 0x75, 0x75, 0x75, 0x75, 0x76, 0x76, 0x76, 0x76, 0x76, 0x77, 0x77, 0x77, 0x77, 0x77, 0x78, 0x78, 0x78, 0x78, 0x78, 0x79, 0x79, 0x79,
    0x79, 0x79, 0x7A, 0x7A, 0x7A, 0x7A, 0x7A, 0x7B, 0x7B, 0x7B, 0x7B, 0x7B, 0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x7D, 0x7D, 0x7D, 0x7D, 0x7D, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x82, 0x82, 0x82, 0x82, 0x82, 0x83, 0x83, 0x83, 0x83, 0x83, 0x84, 0x84, 0x84, 0x84, 0x84, 0x85, 0x85, 0x85, 0x85, 0x85, 0x86, 0x86, 0x86, 0x86, 0x86, 0x87, 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88, 0x88,
    0x88, 0x89, 0x89, 0x89, 0x89, 0x89, 0x8A, 0x8A, 0x8A, 0x8A, 0x8A, 0x8B, 0x8B, 0x8B, 0x8B, 0x8B, 0x8C, 0x8C, 0x8C, 0x8C, 0x8C, 0x8D, 0x8D, 0x8D, 0x8D, 0x8D, 0x8E, 0x8E, 0x8E, 0x8E, 0x8E, 0x8F, 0x8F, 0x8F, 0x8F, 0x8F, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x91, 0x91, 0x91, 0x91, 0x91, 0x92, 0x92, 0x92, 0x92, 0x92, 0x93, 0x93, 0x93, 0x93, 0x93, 0x94, 0x94, 0x94, 0x94, 0x94, 0x95, 0x95, 0x95, 0x95, 0x95, 0x96, 0x96, 0x96, 0x96, 0x96, 0x97, 0x97, 0x97, 0x97, 0x97,
    0x98, 0x98, 0x98, 0x98, 0x98, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9A, 0x9A, 0x9A, 0x9A, 0x9A, 0x9B, 0x9B, 0x9B, 0x9B, 0x9B, 0x9C, 0x9C, 0x9C, 0x9C, 0x9C, 0x9D, 0x9D, 0x9D, 0x9D, 0x9D, 0x9E, 0x9E, 0x9E, 0x9E, 0x9E, 0x9F, 0x9F, 0x9F,
    0x9F, 0x9F, 0xA0, 0xA0, 0xA0, 0xA0, 0xA0, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA2, 0xA2, 0xA2, 0xA2, 0xA2, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA7,
    0xA7, 0xA7, 0xA7, 0xA7, 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, 0xA9, 0xA9, 0xA9, 0xA9, 0xA9, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAC, 0xAC, 0xAC, 0xAC, 0xAC, 0xAD, 0xAD, 0xAD, 0xAD, 0xAD, 0xAE, 0xAE, 0xAE, 0xAE,
    0xAE, 0xAF, 0xAF, 0xAF, 0xAF, 0xAF, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB1, 0xB1, 0xB1, 0xB1, 0xB1, 0xB2, 0xB2, 0xB2, 0xB2, 0xB2, 0xB3, 0xB3, 0xB3, 0xB3, 0xB3, 0x00, 0x03, 0x06, 0x09, 0x0C, 0x0F, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21,
    0x24, 0x27, 0x2A, 0x2D, 0x30, 0x33, 0x36, 0x39, 0x3C, 0x3F, 0x42, 0x45, 0x48, 0x4B, 0x4E, 0x51, 0x54, 0x57, 0x5A, 0x5D, 0x60, 0x02, 0x05, 0x08, 0x0B, 0x0E, 0x11, 0x14,
};

static const uint8_t DELTA_PATCH[954] = {
    0x68, 0x49, 0x00, 0x42, 0xAC, 0x03, 0x02, 0x87, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFA,
    0xFB, 0xFC, 0xFD, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFD, 0xBC, 0x0A, 0x01, 0x13, 0xBB, 0xFD, 0x10, 0x04, 0x07, 0xFE, 0xBA, 0x0D, 0x0E, 0xFB, 0x06, 0x09, 0xBA, 0xBA, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x28, 0x00, 0x03, 0x06, 0x09, 0x0C, 0x0F, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21, 0x24, 0x27, 0x2A, 0x2D, 0x30, 0x33, 0x36, 0x39, 0x3C, 0x3F,
    0x42, 0x45, 0x48, 0x4B, 0x4E, 0x51, 0x54, 0x57, 0x5A, 0x5D, 0x60, 0x02, 0x05, 0x08, 0x0B, 0x0E, 0x11, 0x14,
};

/* ------------------------------------------------------------------------------------------- */

/* The scripted slot serves the BASE from here, because a delta patches what the node is running
   and the scripted slot is standing in for a partition. */
static bool fs_read_base(void *ctx, const uint32_t at, void *const out, const size_t len) {
    (void)ctx;
    if ((size_t)at + len > sizeof(DELTA_BASE))
        return false;
    memcpy(out, &DELTA_BASE[at], len);
    return true;
}

static fake_slot_t g_slot;

static bool fs_begin(void *ctx, const uint32_t size) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_begin++;
    if (s->fail_begin > 0) {
        s->fail_begin--;
        return false;
    }
    s->declared = size;
    s->len = 0u;
    s->open = true;
    s->ended = false;
    return true;
}
static int fs_write(void *ctx, const void *buf, const size_t len) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_write++;
    if (s->fail_write > 0) {
        s->fail_write--;
        return -1;
    }
    if (!s->open || s->len + len > sizeof(s->image))
        return -1;
    memcpy(&s->image[s->len], buf, len);
    s->len += (uint32_t)len;
    return (int)len;
}
static bool fs_end(void *ctx) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_end++;
    if (s->fail_end > 0) {
        s->fail_end--;
        return false;
    }
    s->ended = true;
    return true;
}
static bool fs_activate(void *ctx) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_activate++;
    if (s->fail_activate > 0) {
        s->fail_activate--;
        return false;
    }
    s->active = true;
    return true;
}
static void fs_abort(void *ctx) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_abort++;
    s->open = false;
    s->len = 0u;
}

static const ota_slot_t g_slot_vt = { .ctx = &g_slot, .begin = fs_begin, .write = fs_write, .end = fs_end, .activate = fs_activate, .abort = fs_abort, .read_base = fs_read_base };

/* ------------------------------------------------------------------------------------------- */
/* THE SCRIPTED HASH. A sum, not a digest: deterministic, and the test can poison it.          */

typedef struct {
    uint32_t acc;
    bool poison; /* return a digest that cannot match, to reach the mismatch path */
} fake_hash_t;

static fake_hash_t g_hash;

static void fh_begin(void *ctx) {
    ((fake_hash_t *)ctx)->acc = 0u;
}
static void fh_update(void *ctx, const void *buf, const size_t len) {
    fake_hash_t *const h = (fake_hash_t *)ctx;
    const uint8_t *const p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        h->acc = h->acc * 31u + p[i];
}
static void fh_final(void *ctx, uint8_t *const out, const size_t outlen) {
    fake_hash_t *const h = (fake_hash_t *)ctx;
    memset(out, 0, outlen);
    if (h->poison) {
        memset(out, 0xEE, outlen);
        return;
    }
    for (size_t i = 0; i < 4u && i < outlen; i++)
        out[i] = (uint8_t)(h->acc >> (8u * i));
}

static const ota_hash_t g_hash_vt = { .ctx = &g_hash, .begin = fh_begin, .update = fh_update, .final = fh_final };

/* What the scripted hash will say about a payload, so a bundle can carry a correct PAYLOAD_HASH. */
static void expected_hash(const uint8_t *const p, const uint32_t n, uint8_t *const out) {
    uint32_t acc = 0u;
    for (uint32_t i = 0; i < n; i++)
        acc = acc * 31u + p[i];
    memset(out, 0, IOTDATA_NODE_OTA_HASH_LEN);
    for (size_t i = 0; i < 4u; i++)
        out[i] = (uint8_t)(acc >> (8u * i));
}

/* ------------------------------------------------------------------------------------------- */

static filestore_ram_t g_fs;
static iotdata_node_content_t g_store;
static iotdata_node_ota_apply_t g_apply;

/* WHAT SURVIVES THE REBOOT, which here is simply a buffer that the rig does not clear. The real
   node writes this into NVS or a file; what is under test is the consumer's use of it, so the
   medium is deliberately the dullest thing that persists. `refuse` is the other half of the seam:
   a save that will not land, which on a device is a full or broken store. */
static struct {
    uint8_t blob[64];
    bool refuse;
    int saves;
} g_durable;

static bool durable_save(void *const ctx) {
    (void)ctx;
    g_durable.saves++;
    if (g_durable.refuse)
        return false;
    size_t size = 0u;
    const void *const rec = iotdata_node_content_ota_apply_record(&g_apply, &size);
    memcpy(g_durable.blob, rec, size);
    return true;
}

static const ota_record_t g_record_vt = { .ctx = NULL, .save = durable_save };

/* THE CODEC IS THE SHIPPING ONE, deliberately. A scripted inflater would prove the seam is called
   and nothing else; the bugs worth catching in a streaming decompressor are about input that runs
   out mid-symbol and an output buffer that fills before the input is spent, and only a real one has
   them. g_codec_vt is set to NULL in the one test that needs a build WITHOUT a decoder. */
static inflate_linux_t g_inflate;
static const ota_codec_t *g_codec_vt = NULL;

/* THE PATCHER IS ALSO THE SHIPPING ONE -- d_module_hpatch.h is platform-free, so the suite runs the
   same HPatchLite the relay runs, against patches cut by the same hdiffi the bundler calls. NULL in
   the tests that need a build which cannot take a delta at all. */
static hpatch_t g_patchst;
static const ota_patch_t *g_patch_vt = NULL;

#define MY_APP   "relay"
#define MY_STAMP "202610110000"

static const iotdata_node_ota_device_t g_device = {
    .app = MY_APP,
    .stamp = MY_STAMP,
    .bootloader = 1u,
    .slot_size = 4096u,
    .base_hash = NULL,
    .codec_deflate = false,
    .codec_heatshrink = false,
};

/* A bundle, built the way ota-bundle builds one: magic, a kvr header, END, then the payload. */
static uint32_t bundle(const uint32_t id, const char *const app, const char *const stamp, const uint8_t form, const uint8_t flags, const uint32_t paylen, const bool good_hash) {
    static uint8_t blob[3072];
    static uint8_t payload[2048];
    for (uint32_t i = 0; i < paylen && i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 7u + 3u);

    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, stamp);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, form);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, IOTDATA_NODE_OTA_CODEC_NONE);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, flags);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, paylen);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, paylen);
    uint8_t ph[IOTDATA_NODE_OTA_HASH_LEN];
    expected_hash(payload, paylen, ph);
    if (!good_hash)
        ph[0] = (uint8_t)(ph[0] ^ 0xFFu);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_HASH, ph, IOTDATA_NODE_OTA_HASH_LEN);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    memcpy(&blob[head], payload, paylen);
    return filestore_ram_plant(&g_fs, id, blob, head + paylen) ? head : 0u;
}

/* A FULL bundle that says it is DEFLATE, for the case iotdata_node_ota_check cannot refuse: the
   codec a device declares is the device's business, so a build advertising codec_deflate gets this
   waved through -- and the consumer has no inflate loop to meet it with. The payload here is not
   really compressed, which does not matter: nothing on the refusal path ever decodes it, and the
   point is that every FORMAT check passes. output_size is larger than the wire payload, as a real
   compressed bundle's would be. */
static uint32_t bundle_coded(const uint32_t id, const char *const app, const char *const stamp, const uint8_t codec, const uint32_t paylen) {
    static uint8_t blob[3072];
    static uint8_t payload[2048];
    for (uint32_t i = 0; i < paylen && i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 11u + 5u);
    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, stamp);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, IOTDATA_NODE_OTA_FORM_FULL);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, codec);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, 0u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    /* output_size > payload only makes sense WITH a codec; uncompressed, the two are the same thing
       by definition, and claiming otherwise is a bundle the consumer is right to refuse. */
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, (codec == IOTDATA_NODE_OTA_CODEC_NONE) ? paylen : paylen * 2u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, paylen);
    uint8_t ph[IOTDATA_NODE_OTA_HASH_LEN];
    expected_hash(payload, paylen, ph);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_HASH, ph, IOTDATA_NODE_OTA_HASH_LEN);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    memcpy(&blob[head], payload, paylen);
    return filestore_ram_plant(&g_fs, id, blob, head + paylen) ? head : 0u;
}

/* A FULL + DEFLATE bundle whose payload is a real raw-deflate stream, compressed here by the same
   zlib the bundler uses (deflateRawSync == deflateInit2 with negative window bits). The image it
   decompresses to is handed back through `want`/`wantlen` so a test can compare the slot against
   it byte for byte -- which is the only check that proves the inflater was actually run rather than
   merely called. */
static uint32_t bundle_deflated(const uint32_t id, const char *const app, const char *const stamp, const uint32_t imagelen, uint8_t *const want, const size_t wantlen, const uint32_t drop) {
    static uint8_t blob[8192];
    static uint8_t image[4096];
    static uint8_t comp[8192];
    if (imagelen > sizeof(image) || imagelen > wantlen)
        return 0u;
    /* compressible, but not trivially so: runs the inflater through both literals and matches */
    for (uint32_t i = 0; i < imagelen; i++)
        image[i] = (uint8_t)((i / 7u) % 11u);
    memcpy(want, image, imagelen);

    z_stream z;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0u;
    z.next_in = image;
    z.avail_in = (uInt)imagelen;
    z.next_out = comp;
    z.avail_out = (uInt)sizeof(comp);
    const int rc = deflate(&z, Z_FINISH);
    const uint32_t complen = (uint32_t)(sizeof(comp) - z.avail_out);
    (void)deflateEnd(&z);
    if (rc != Z_STREAM_END || complen == 0u)
        return 0u;

    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, stamp);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, IOTDATA_NODE_OTA_FORM_FULL);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, IOTDATA_NODE_OTA_CODEC_DEFLATE);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, 0u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, imagelen);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, complen);
    uint8_t ph[IOTDATA_NODE_OTA_HASH_LEN];
    expected_hash(comp, complen, ph); /* the WIRE payload, as the bundler hashes it */
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_HASH, ph, IOTDATA_NODE_OTA_HASH_LEN);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    memcpy(&blob[head], comp, complen);
    /* `drop` plants FEWER payload bytes than PAYLOAD_SIZE claims -- the header is untouched, so this
       is the ordinary truncation a lost tail produces, not a doctored header. */
    if (drop >= complen)
        return 0u;
    return filestore_ram_plant(&g_fs, id, blob, head + complen - drop) ? head : 0u;
}

/* A DELTA BUNDLE carrying the embedded hdiffi patch as its payload, optionally squeezed by the
   codec -- which is the shipping combination: hdiffi uncompressed, deflate around it. */
/* A codec value only this file knows: the HEADER says deflate (so a codec is in the path) while the
   payload is planted uncompressed (so an identity-transform double can stand in for one). */
#define IOTDATA_NODE_OTA_CODEC_DEFLATE_IDENTITY 0xFEu

static uint32_t bundle_delta(const uint32_t id, const char *const app, const char *const stamp, const uint8_t codec, const uint8_t *const basehash) {
    static uint8_t blob[4096];
    static uint8_t wire[4096];
    uint32_t wirelen = (uint32_t)sizeof(DELTA_PATCH);
    memcpy(wire, DELTA_PATCH, wirelen);
    if (codec == IOTDATA_NODE_OTA_CODEC_DEFLATE) {
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return 0u;
        z.next_in = (Bytef *)(uintptr_t)DELTA_PATCH;
        z.avail_in = (uInt)sizeof(DELTA_PATCH);
        z.next_out = wire;
        z.avail_out = (uInt)sizeof(wire);
        const int rc = deflate(&z, Z_FINISH);
        wirelen = (uint32_t)(sizeof(wire) - z.avail_out);
        (void)deflateEnd(&z);
        if (rc != Z_STREAM_END)
            return 0u;
    }
    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, stamp);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, IOTDATA_NODE_OTA_FORM_DELTA);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, (codec == IOTDATA_NODE_OTA_CODEC_DEFLATE_IDENTITY) ? (uint8_t)IOTDATA_NODE_OTA_CODEC_DEFLATE : codec);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, 0u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, (uint32_t)sizeof(DELTA_NEW));
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, wirelen);
    if (basehash != NULL)
        iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_BASE_HASH, basehash, IOTDATA_NODE_OTA_HASH_LEN);
    uint8_t ph[IOTDATA_NODE_OTA_HASH_LEN];
    expected_hash(wire, wirelen, ph);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_HASH, ph, IOTDATA_NODE_OTA_HASH_LEN);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    memcpy(&blob[head], wire, wirelen);
    return filestore_ram_plant(&g_fs, id, blob, head + wirelen) ? head : 0u;
}

/* What iotdata_node_ota_check makes of a stored bundle, so a test can assert that the FORMAT half
   passes something the CONSUMER half must still refuse. */
static bool iotdata_node_ota_check_passes(const uint32_t id, const iotdata_node_ota_device_t *const dev) {
    uint8_t peek[IOTDATA_NODE_CONTENT_BLOCK_BYTES];
    uint32_t total = 0u, held = 0u;
    if (!iotdata_node_content_stat(&g_store, id, &total, &held))
        return false;
    const uint32_t want = (held < (uint32_t)sizeof(peek)) ? held : (uint32_t)sizeof(peek);
    if (iotdata_node_content_read(&g_store, id, 0u, peek, want) != (int)want)
        return false;
    iotdata_node_ota_header_t h;
    if (iotdata_node_ota_parse(peek, (size_t)want, &h) != IOTDATA_NODE_OTA_OK)
        return false;
    return iotdata_node_ota_check(&h, dev, (size_t)total) == IOTDATA_NODE_OTA_OK;
}

/* A fresh everything, as a restarted process has -- except the durable record, which is the point. */
static void rig_with(const bool keep_state, const bool enabled, const char *const my_stamp, const iotdata_node_ota_device_t *const dev) {
    memset(&g_slot, 0, sizeof(g_slot));
    memset(&g_hash, 0, sizeof(g_hash));
    if (!keep_state)
        memset(&g_durable, 0, sizeof(g_durable));
    g_durable.refuse = false;
    (void)filestore_ram_init(&g_fs);
    iotdata_node_content_init(&g_store, &g_fs.backend);
    CHECK(iotdata_node_content_ota_apply_begin(&g_apply, &g_store, &g_record_vt, dev, &g_slot_vt, &g_hash_vt, g_codec_vt, g_patch_vt, my_stamp, enabled), "the consumer registers");
    /* the load, which on a device is iotdata_node_state_load putting the block back */
    size_t size = 0u;
    void *const rec = iotdata_node_content_ota_apply_record(&g_apply, &size);
    memcpy(rec, g_durable.blob, size);
}

static void rig_as(const bool keep_state, const bool enabled, const char *const my_stamp) {
    rig_with(keep_state, enabled, my_stamp, &g_device);
}

static void rig(const bool keep_state, const bool enabled) {
    rig_as(keep_state, enabled, MY_STAMP);
}

/* Drive the store until it stops having work, bounded so a stuck consumer fails rather than hangs. */
static iotdata_node_content_event_t run(const uint32_t now_ms, const int max) {
    iotdata_node_content_event_t last = IOTDATA_NODE_CONTENT_EVENT_NONE;
    for (int i = 0; i < max; i++) {
        const iotdata_node_content_event_t ev = iotdata_node_content_tick(&g_store, now_ms);
        if (ev != IOTDATA_NODE_CONTENT_EVENT_NONE)
            last = ev;
        if (ev == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED || ev == IOTDATA_NODE_CONTENT_EVENT_WORK_DONE)
            break;
    }
    return last;
}

/* ------------------------------------------------------------------------------------------- */

static void test_not_ours(void) {
    printf("\nwhat it leaves alone, which is most of what a store holds\n");
    rig(false, true);
    uint8_t plain[200];
    for (unsigned i = 0; i < sizeof(plain); i++)
        plain[i] = (uint8_t)i;
    CHECK(filestore_ram_plant(&g_fs, 0x053701u, plain, sizeof(plain)), "an ordinary file");
    CHECK(bundle(0x053702u, "gateway", "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 100u, true) > 0u, "and a bundle for another application");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 2, "both are offered");
    CHECK(g_store.owns == 0u, "and NEITHER is claimed -- not firmware, and not ours");
    CHECK(g_slot.n_begin == 0, "nothing went near the slot");
    iotdata_node_content_end(&g_store);
}

static void test_applies(void) {
    printf("\na bundle for this build is written, verified and activated\n");
    rig(false, true);
    CHECK(bundle(0x053711u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 1500u, true) > 0u, "a newer bundle");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "and claimed by the firmware consumer");
    CHECK(g_slot.n_begin == 0, "but claim() touches no flash: the slot is opened by work()");

    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "ticking it through asks for a restart");
    CHECK(g_slot.len == 1500u, "the whole payload reached the slot");
    CHECK(g_slot.declared == 1500u, "opened at the size the header declared");
    CHECK(g_slot.ended && g_slot.active, "finalised, and then selected for the next boot");
    CHECK(g_slot.n_write == 6, "in six pieces of 256 -- the loop is never held for a whole image");
    CHECK(g_apply.stat_applied == 1u, "counted");

    /* and the bytes are the payload, not the header */
    CHECK(g_slot.image[0] == 3u && g_slot.image[1] == 10u, "and they are the PAYLOAD, from after the header");

    /* THE ATTEMPT WAS WRITTEN DOWN, and before the activate rather than after it */
    CHECK(g_apply.persist.id == 0x053711u, "the attempt names the bundle");
    CHECK(g_apply.persist.attempts == 1u, "the first attempt");
    CHECK(strcmp(g_apply.persist.stamp, "202610120000") == 0, "and the stamp it is trying to run");
    iotdata_node_content_end(&g_store);
}

static void test_the_record_precedes_the_activate(void) {
    printf("\nthe attempt is recorded BEFORE the slot is activated, not after\n");
    rig(false, true);
    CHECK(bundle(0x053721u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 600u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    /* A STORE THAT WILL NOT WRITE is how the order becomes observable: if the record were written
       after the activate, a failed flush would leave a node booting an image nobody counted. */
    g_durable.refuse = true; /* the durable store will not take it, so the record does not land */
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "a flush that fails does NOT lead to a reboot");
    CHECK(!g_slot.active, "and the slot was never activated");
    CHECK(g_slot.n_activate == 0, "not even attempted");
    iotdata_node_content_end(&g_store);
}

static void test_slot_refusals(void) {
    printf("\nevery way the slot can refuse, and none of them boots anything\n");
    const char *const names[] = { "begin", "write", "end", "activate" };
    for (int which = 0; which < 4; which++) {
        rig(false, true);
        g_slot.fail_begin = (which == 0) ? 1 : 0;
        g_slot.fail_write = (which == 1) ? 1 : 0;
        g_slot.fail_end = (which == 2) ? 1 : 0;
        g_slot.fail_activate = (which == 3) ? 1 : 0;
        CHECK(bundle(0x053731u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 800u, true) > 0u, names[which]);
        (void)iotdata_node_content_begin(&g_store, 1000u);
        CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "refused somewhere, and no restart asked for");
        CHECK(!g_slot.active, "and nothing was selected to boot");
        CHECK(g_apply.stat_rejected == 1u, "counted as a rejection");
        /* AND IT IS NOT TRIED AGAIN THIS BOOT: a slot that just refused will refuse again, and a
           consumer that kept trying would spend the node's life writing flash. */
        const int writes = g_slot.n_write;
        (void)run(2000u, 20);
        CHECK(g_slot.n_write == writes, "and not retried for the rest of this boot");
        iotdata_node_content_end(&g_store);
    }
}

static void test_payload_hash_mismatch(void) {
    printf("\nthe right number of the wrong bytes is a file that gets destroyed\n");
    rig(false, true);
    CHECK(bundle(0x053741u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 700u, false) > 0u, "a bundle whose PAYLOAD_HASH does not match its payload");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "it is not applied");
    CHECK(!g_slot.active, "nothing is selected to boot");
    CHECK(g_slot.n_abort == 1, "the half-written slot is abandoned");
    /* The store's own CRC already passed, so this is not a transfer fault -- the file is not what
       it says it is, and keeping it would mean trying it again for ever. */
    CHECK(!iotdata_node_content_stat(&g_store, 0x053741u, NULL, NULL), "and the file is DESTROYED, not kept");
    iotdata_node_content_end(&g_store);
}

static void test_claimed_but_refused(void) {
    printf("\na bundle plainly for this node, and plainly not takeable, is claimed and explained\n");
    rig(false, true);
    /* older, with no downgrade flag: a POLICY refusal, and the one that can stop being one */
    CHECK(bundle(0x053751u, MY_APP, "202610100000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 300u, true) > 0u, "an older bundle");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "CLAIMED -- so inspect has somebody to ask");
    CHECK(g_slot.n_begin == 0, "and never written");
    char what[160];
    CHECK(iotdata_node_content_describe(&g_store, 0x053751u, what, sizeof(what)) > 0, "it describes itself");
    CHECK(strstr(what, "older") != NULL, "naming the reason it is sitting there");
    CHECK(iotdata_node_content_stat(&g_store, 0x053751u, NULL, NULL), "AND IT IS KEPT: a refusal can stop being one");

    /* the same bundle re-cut with the flag IS taken, which is what makes keeping it worthwhile */
    rig(false, true);
    CHECK(bundle(0x053752u, MY_APP, "202610100000", IOTDATA_NODE_OTA_FORM_FULL, IOTDATA_NODE_OTA_FLAG_DOWNGRADE, 300u, true) > 0u, "re-cut with the downgrade flag");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and the same downgrade is applied");
    iotdata_node_content_end(&g_store);
}

/* test_delta_is_refused_for_now() used to live here, asserting that a delta -- even one whose base
   matched -- never reached the slot, because there was no patcher. There is one now, so the question
   it asked has moved: test_a_delta_needs_a_patcher() keeps the half that still matters (a build
   WITHOUT a patcher must still refuse, and must not trust iotdata_node_ota_check's verdict on a
   form it cannot apply), and test_a_delta_is_patched_onto_the_running_image() covers the rest. */

static void test_a_codec_we_cannot_decode_is_refused(void) {
    printf("\na codec the consumer cannot decode is refused, even where the DEVICE says it can\n");

    /* The device claims deflate, as the relay does -- tinfl is in the C3's ROM, so the claim is
       true of the hardware. It is not true of this consumer, which has no inflate loop. */
    static iotdata_node_ota_device_t deflating;
    deflating = g_device;
    deflating.codec_deflate = true;

    /* SAID OUT LOUD, not inherited. These are globals, and this test is about their being absent --
       so it sets them, rather than depending on whatever ran before it. A test that ran earlier and
       left a codec behind made this one pass a bundle it is supposed to refuse, and the failure
       surfaced here rather than where the state was set. */
    g_codec_vt = NULL;
    g_patch_vt = NULL;

    rig_with(false, true, MY_STAMP, &deflating);
    CHECK(bundle_coded(0x053771u, MY_APP, "202610120000", IOTDATA_NODE_OTA_CODEC_DEFLATE, 300u) > 0u, "a FULL bundle, deflate, newer, every hash right");
    CHECK(iotdata_node_ota_check_passes(0x053771u, &deflating), "and iotdata_node_ota_check passes it -- it believes the device about codecs");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "claimed: it is firmware for us, we simply cannot take it");
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "NOT applied");
    CHECK(g_slot.n_begin == 0, "and not one compressed byte reached the slot");
    char what[160];
    (void)iotdata_node_content_describe(&g_store, 0x053771u, what, sizeof(what));
    CHECK(strstr(what, "codec") != NULL, "and the reason given is the codec");
    iotdata_node_content_end(&g_store);

    /* THE SAME BUNDLE UNCOMPRESSED IS TAKEN, which is what makes the refusal above about the codec
       and not about anything else the bundle happens to be. */
    rig_with(false, true, MY_STAMP, &deflating);
    CHECK(bundle_coded(0x053772u, MY_APP, "202610120000", IOTDATA_NODE_OTA_CODEC_NONE, 300u) > 0u, "the same bundle, codec none");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied");
    CHECK(g_slot.n_begin == 1, "the slot was opened this time");
    iotdata_node_content_end(&g_store);
}

static void test_deflate_is_inflated_into_the_slot(void) {
    printf("\na deflate payload is INFLATED into the slot, byte for byte\n");

    static iotdata_node_ota_device_t deflating;
    deflating = g_device;
    deflating.codec_deflate = true;

    /* the shipping codec, for this test and the next */
    g_codec_vt = inflate_linux_init(&g_inflate);

    static uint8_t want[4096];
    const uint32_t imagelen = 3000u;
    rig_with(false, true, MY_STAMP, &deflating);
    CHECK(bundle_deflated(0x053781u, MY_APP, "202610120000", imagelen, want, sizeof(want), 0u) > 0u, "a real raw-deflate bundle");
    CHECK(iotdata_node_ota_check_passes(0x053781u, &deflating), "it passes the format checks");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 200) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied, restart asked for");
    CHECK(g_slot.len == imagelen, "the slot holds exactly output_size bytes, not the payload's length");
    CHECK(memcmp(g_slot.image, want, imagelen) == 0, "AND THEY ARE THE IMAGE, which is the only check that proves it inflated");
    CHECK(g_slot.active, "and it was activated");

    /* THE COMPRESSED PAYLOAD WAS SMALLER THAN WHAT LANDED, so the two counters really are different
       quantities and the test is not accidentally passing on an identity transform. */
    CHECK(g_apply.stat_written < g_slot.len, "fewer payload bytes were consumed than image bytes produced");
    iotdata_node_content_end(&g_store);
}

static void test_a_truncated_deflate_stream_is_caught(void) {
    printf("\na deflate stream that stops mid-symbol is caught, and is not a hash failure\n");

    static iotdata_node_ota_device_t deflating;
    deflating = g_device;
    deflating.codec_deflate = true;
    g_codec_vt = inflate_linux_init(&g_inflate);

    static uint8_t want[4096];
    rig_with(false, true, MY_STAMP, &deflating);
    /* the last 40 payload bytes never arrived, and PAYLOAD_SIZE still says they should have */
    CHECK(bundle_deflated(0x053782u, MY_APP, "202610120000", 3000u, want, sizeof(want), 40u) > 0u, "a deflate bundle missing the end of its stream");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 200) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "NOT applied");
    CHECK(!g_slot.active, "nothing was activated");
    iotdata_node_content_end(&g_store);
}

static void test_the_count_belongs_to_the_bundle(void) {
    printf("\nthe attempt count is per BUNDLE: a new one does not inherit the last one's failures\n");

    /* burn the limit on one bundle */
    for (int boot = 1; boot <= 3; boot++) {
        rig(boot > 1, true);
        CHECK(bundle(0x053791u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 400u, true) > 0u, "the bad bundle");
        (void)iotdata_node_content_begin(&g_store, 1000u);
        CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "tried");
        iotdata_node_content_end(&g_store);
    }
    CHECK(g_apply.persist.attempts == 3u, "three attempts spent on it");

    /* the limit holds for THAT bundle */
    rig(true, true);
    CHECK(bundle(0x053791u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 400u, true) > 0u, "still there");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and is not tried a fourth time");
    iotdata_node_content_end(&g_store);

    /* A DIFFERENT BUNDLE IS A DIFFERENT QUESTION. This is the case that was wrong: the count carried
       over, so the replacement arrived already condemned -- reported as "attempt 4, giving up" on
       the very boot where it had just run. */
    rig(true, true);
    CHECK(bundle(0x053792u, MY_APP, "202610120001", IOTDATA_NODE_OTA_FORM_FULL, 0u, 400u, true) > 0u, "a DIFFERENT bundle, newer still");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "is applied");
    CHECK(g_apply.persist.id == 0x053792u, "and the record now names it");
    CHECK(g_apply.persist.attempts == 1u, "AT ATTEMPT ONE, not four");
    iotdata_node_content_end(&g_store);
}

static void test_a_deliberate_rollback_is_not_a_retry(void) {
    printf("\nan abandoned image is NOT re-applied: a rollback by hand is a verdict, not a crash\n");
#define NEW_STAMP "202610120000"

    /* boot 1, running OUR build: a newer bundle arrives and is applied */
    rig(false, true);
    CHECK(bundle(0x0537a1u, MY_APP, NEW_STAMP, IOTDATA_NODE_OTA_FORM_FULL, 0u, 400u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied");
    CHECK(g_apply.persist.attempts == 1u, "one attempt recorded");
    iotdata_node_content_end(&g_store);

    /* boot 2, running THE NEW IMAGE: it boots and the bootloader confirms it. This is the case that
       bit -- confirmation ends the attempt, so an abandon that looked only for an attempt in flight
       found nothing to condemn, and the node re-applied what it had just been rolled back from. */
    rig_as(true, true, NEW_STAMP);
    iotdata_node_content_ota_apply_confirmed(&g_apply);
    CHECK(g_apply.persist.attempts == 0u, "the attempt is over");
    CHECK(g_apply.persist.id == 0x0537a1u, "and the bundle behind what we are running is remembered");

    /* the operator decides it is bad and rolls back -- condemned BEFORE the reboot, because the
       rollback call does not return when it works */
    iotdata_node_content_ota_apply_abandon(&g_apply);
    CHECK(g_apply.persist.attempts == (uint8_t)IOTDATA_NODE_OTA_APPLY_ATTEMPTS_MAX, "the bundle is spent, not merely counted");
    iotdata_node_content_end(&g_store);

    /* boot 3, back on OUR build: the bundle is still newer, still held, and must be left alone */
    rig(true, true);
    CHECK(bundle(0x0537a1u, MY_APP, NEW_STAMP, IOTDATA_NODE_OTA_FORM_FULL, 0u, 400u, true) > 0u, "the bundle is still held");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and NOT applied again");
    CHECK(g_slot.n_begin == 0, "nothing was written");
    char what[160];
    (void)iotdata_node_content_describe(&g_store, 0x0537a1u, what, sizeof(what));
    CHECK(strstr(what, "did not run") != NULL, "and inspect says so");

    /* BUT SOMEBODY WHO CHANGES THEIR MIND CAN ARM IT AGAIN, which is the other half of the deal: a
       verdict, not a destruction. */
    CHECK(iotdata_node_content_ota_apply_retry(&g_apply, 0x0537a1u), "armed again by hand");
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and then it IS applied");
    iotdata_node_content_end(&g_store);
#undef NEW_STAMP
}

/* ------------------------------------------------------------------------------------------- */
/* AN EAGER CODEC, which is a test double for a REAL difference and not a convenience.            */
/*                                                                                                */
/* zlib consumes lazily: hand it a kilobyte of input and ask for 64 bytes of output and it takes   */
/* only as much input as those 64 bytes needed. The esp32c3's ROM tinfl consumes EAGERLY and       */
/* buffers into a 32KB window, so it can swallow an entire payload on the first call and hold the  */
/* output. Both obey ota_codec_t. Only the lazy one forgave a read_patch that stopped at the last  */
/* payload byte -- which is why a delta that applied perfectly on a host failed on a relay with    */
/* `unsupported form`. This double behaves like the eager one, so the suite can see it too.        */
/*                                                                                                */
/* It is an identity transform: payload bytes in, the same bytes out, just not at the same time.   */

typedef struct {
    ota_codec_t _ota_codec;
    uint8_t held[8192];
    size_t have, at;
    bool open;
} eager_codec_t;

static bool eager_begin(void *ctx) {
    eager_codec_t *const s = (eager_codec_t *)ctx;
    s->have = s->at = 0u;
    s->open = true;
    return true;
}
static int eager_step(void *ctx, const void *in, size_t inlen, size_t *consumed, void *out, size_t outlen) {
    eager_codec_t *const s = (eager_codec_t *)ctx;
    *consumed = 0u;
    /* EVERYTHING, AT ONCE -- and then dribble it back a little per call */
    if (inlen > 0u && s->have + inlen <= sizeof(s->held)) {
        memcpy(&s->held[s->have], in, inlen);
        s->have += inlen;
        *consumed = inlen;
    }
    size_t give = s->have - s->at;
    if (give > outlen)
        give = outlen;
    if (give > 32u)
        give = 32u; /* a trickle, so the drain has to be asked many times */
    memcpy(out, &s->held[s->at], give);
    s->at += give;
    return (int)give;
}
static bool eager_end(void *ctx) {
    eager_codec_t *const s = (eager_codec_t *)ctx;
    s->open = false;
    return s->at == s->have; /* everything we swallowed was handed back */
}
static void eager_abort(void *ctx) {
    ((eager_codec_t *)ctx)->open = false;
}

static const ota_codec_t *eager_codec_init(eager_codec_t *const s) {
    memset(s, 0, sizeof(*s));
    s->_ota_codec = (const ota_codec_t){ .ctx = s, .begin = eager_begin, .step = eager_step, .end = eager_end, .abort = eager_abort };
    return &s->_ota_codec;
}

/* ------------------------------------------------------------------------------------------- */

static void test_a_delta_is_patched_onto_the_running_image(void) {
    printf("\na DELTA is patched onto the running image, byte for byte, through the real engine\n");

    /* A DELTA MUST NAME ITS BASE and the device must agree, which iotdata_node_ota_check enforces --
       so the bundle carries BASE_HASH and the device says that is what it is running. Without it the
       check refuses with NO_BASE, correctly: a patch against an unstated base is a patch against
       anything. */
    static const uint8_t base_hash[IOTDATA_NODE_OTA_HASH_LEN] = { 0x5B, 0xA5, 0xE0, 0x01 };
    static iotdata_node_ota_device_t dev;
    dev = g_device;
    dev.codec_deflate = true;
    dev.base_hash = base_hash;
    g_codec_vt = inflate_linux_init(&g_inflate);
    g_patch_vt = ota_patch_init(&g_patchst);

    /* the shipping combination: an uncompressed hdiffi patch with deflate around it */
    rig_with(false, true, MY_STAMP, &dev);
    CHECK(bundle_delta(0x0537b1u, MY_APP, "202610120000", IOTDATA_NODE_OTA_CODEC_DEFLATE, base_hash) > 0u, "a delta bundle, deflated");
    CHECK(iotdata_node_ota_check_passes(0x0537b1u, &dev), "it passes the format checks");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 200) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied, restart asked for");
    CHECK(g_slot.len == sizeof(DELTA_NEW), "the slot holds the NEW image's length, not the patch's");
    CHECK(memcmp(g_slot.image, DELTA_NEW, sizeof(DELTA_NEW)) == 0, "AND IT IS THE NEW IMAGE -- patched from a base it never received");
    CHECK(g_slot.active, "and activated");
    /* the patch on the wire was smaller than the image it produced, which is the entire point */
    CHECK(g_apply.stat_written < g_slot.len, "fewer payload bytes arrived than image bytes were written");
    iotdata_node_content_end(&g_store);

    /* AND UNCOMPRESSED, so the refusal above is about the codec and this is about the form */
    rig_with(false, true, MY_STAMP, &dev);
    CHECK(bundle_delta(0x0537b2u, MY_APP, "202610120000", IOTDATA_NODE_OTA_CODEC_NONE, base_hash) > 0u, "the same delta, codec none");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 200) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied");
    CHECK(memcmp(g_slot.image, DELTA_NEW, sizeof(DELTA_NEW)) == 0, "and produced the same image");
    iotdata_node_content_end(&g_store);
}

static void test_a_delta_needs_a_patcher(void) {
    printf("\nwithout a patcher a delta is refused, however good it looks\n");
    g_patch_vt = NULL; /* a build that takes whole images only */
    g_codec_vt = NULL;
    static const uint8_t base_hash[IOTDATA_NODE_OTA_HASH_LEN] = { 0x5B, 0xA5, 0xE0, 0x01 };
    static iotdata_node_ota_device_t nopatch;
    nopatch = g_device;
    nopatch.base_hash = base_hash;
    rig_with(false, true, MY_STAMP, &nopatch);
    /* every check passes -- the base is the one we are running -- so the refusal can only be the
       form, which is the half of the old test that still matters */
    CHECK(bundle_delta(0x0537b3u, MY_APP, "202610120000", IOTDATA_NODE_OTA_CODEC_NONE, base_hash) > 0u, "a perfectly good delta");
    CHECK(iotdata_node_ota_check_passes(0x0537b3u, &nopatch), "and iotdata_node_ota_check passes it");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "claimed: it is firmware for us, we simply cannot take it");
    CHECK(run(1000u, 200) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "NOT applied");
    CHECK(g_slot.n_begin == 0, "and not one byte reached the slot");
    char what[160];
    (void)iotdata_node_content_describe(&g_store, 0x0537b3u, what, sizeof(what));
    CHECK(strstr(what, "form") != NULL, "and the reason given is the form");
    iotdata_node_content_end(&g_store);
}

/* THE TRAP THE EMBEDDED-VTABLE PATTERN SETS, pinned so it cannot be walked into twice.
 *
 * ota_codec_t and ota_patch_t live at the top of the structs that implement them, and the consumer
 * holds a pointer INTO that struct. So any method which resets its own instance with
 * memset(s, 0, sizeof *s) destroys the vtable its caller is using -- and nothing says so: begin()
 * returns true, and the next step() jumps to address zero. That is exactly what happened to
 * d_module_inflate_linux.h, and the symptom was a segfault three frames away inside the patcher,
 * which is the last place anyone would look.
 *
 * A method may reset its own state. The vtable belongs to init(). */
static void test_a_seam_survives_its_own_begin(void) {
    printf("\nan implementation's vtable survives its own begin() -- it is not the method's to zero\n");

    static inflate_linux_t codec;
    const ota_codec_t *const cvt = inflate_linux_init(&codec);
    CHECK(cvt != NULL && cvt->step != NULL && cvt->ctx != NULL, "init hands back a usable codec");
    const void *const step_was = (const void *)(uintptr_t)cvt->step;
    CHECK(cvt->begin(cvt->ctx), "it begins");
    CHECK((const void *)(uintptr_t)cvt->step == step_was, "and step is STILL the same function afterwards");
    CHECK(cvt->ctx == (void *)&codec, "and ctx still points at the instance");
    cvt->abort(cvt->ctx);

    static hpatch_t patcher;
    const ota_patch_t *const pvt = ota_patch_init(&patcher);
    CHECK(pvt != NULL && pvt->apply != NULL && pvt->ctx == (void *)&patcher, "and the patcher's vtable is wired the same way");
}

static void test_the_codec_is_drained_after_the_payload_runs_out(void) {
    printf("\nthe codec is DRAINED after the last payload byte -- running out of payload is not running out of patch\n");

    static const uint8_t base_hash[IOTDATA_NODE_OTA_HASH_LEN] = { 0x5B, 0xA5, 0xE0, 0x01 };
    static iotdata_node_ota_device_t dev;
    dev = g_device;
    dev.codec_deflate = true;
    dev.base_hash = base_hash;

    /* AN EAGER CODEC over an UNCOMPRESSED patch: the identity transform means the patch bytes are
       the payload bytes, so the only thing under test is whether read_patch keeps asking after the
       payload is spent. Declared CODEC_DEFLATE so the codec is in the path at all. */
    static eager_codec_t eager;
    g_codec_vt = eager_codec_init(&eager);
    g_patch_vt = ota_patch_init(&g_patchst);

    rig_with(false, true, MY_STAMP, &dev);
    CHECK(bundle_delta(0x0537c1u, MY_APP, "202610120000", IOTDATA_NODE_OTA_CODEC_DEFLATE_IDENTITY, base_hash) > 0u, "a delta whose codec swallows the lot in one gulp");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 4000) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied anyway");
    CHECK(g_slot.len == sizeof(DELTA_NEW), "the whole image was produced");
    CHECK(memcmp(g_slot.image, DELTA_NEW, sizeof(DELTA_NEW)) == 0, "and it is correct, drained out of the codec after the payload ended");
    iotdata_node_content_end(&g_store);
}

static void test_disabled(void) {
    printf("\nswitched off, it holds a bundle without running it\n");
    rig(false, false);
    CHECK(bundle(0x053771u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 400u, true) > 0u, "a perfectly good bundle");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "and claimed -- holding is not running");
    CHECK(run(1000u, 10) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "but not applied");
    CHECK(g_slot.n_begin == 0, "the slot is untouched");
    char what[160];
    (void)iotdata_node_content_describe(&g_store, 0x053771u, what, sizeof(what));
    CHECK(strstr(what, "switched off") != NULL, "and says why");

    /* Turned on at run time it takes the bundle without a restart -- but on the store's RE-OFFER
       cadence, not instantly, because an owner that answered a bare MINE is asked again later and
       that is the only moment its answer can change. */
    iotdata_node_content_ota_apply_enable(&g_apply, true);
    CHECK(run(1000u + 1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "a second later, nothing has been asked again");
    const uint32_t after_reoffer = 1000u + (uint32_t)IOTDATA_NODE_CONTENT_REOFFER_S * 1000u + 1u;
    CHECK(run(after_reoffer, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and on the next re-offer it is applied");
    iotdata_node_content_end(&g_store);
}

/* ------------------------------------------------------------------------------------------- */
/* THE ATTEMPT COUNTER ACROSS REBOOTS. The datastore is kept, everything else is rebuilt --     */
/* which is exactly what a restart is.                                                          */

static void test_attempt_survives_and_is_judged(void) {
    printf("\nthe attempt counter, across the reboot it was written for\n");

    /* boot 1: apply it */
    rig(false, true);
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied, restart asked for");
    iotdata_node_content_end(&g_store);

    /* boot 2: the OLD image is running again -- it did not stick. */
    rig(true, true);
    char line[160];
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the record survived the restart");
    CHECK(strstr(line, "did NOT run") != NULL, "and the stamp is not ours, so it did not run");
    CHECK(strstr(line, "try again") != NULL, "with one go left");
    CHECK(g_apply.persist.attempts == 1u, "one attempt so far");
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "the bundle is still there");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "so it is tried again");
    CHECK(g_apply.persist.attempts == 2u, "and counted again");
    iotdata_node_content_end(&g_store);

    /* boot 3: still the old image. The third attempt is the last. */
    rig(true, true);
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "still there");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "the third attempt is allowed");
    CHECK(g_apply.persist.attempts == 3u, "three");
    iotdata_node_content_end(&g_store);

    /* boot 4: THE LIMIT. It is refused, kept, and says so. */
    rig(true, true);
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the record is still there");
    CHECK(strstr(line, "giving up") != NULL, "and the verdict is to stop");
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "the bundle too");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "and claimed, because it is still ours");
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "but NOT applied a fourth time");
    CHECK(g_slot.n_begin == 0, "nothing written");
    char what[160];
    (void)iotdata_node_content_describe(&g_store, 0x053781u, what, sizeof(what));
    CHECK(strstr(what, "did not run") != NULL, "and inspect explains it");
    iotdata_node_content_end(&g_store);
}

/* THE ORDER A REAL NODE IS FORCED INTO, which is not the one rig() uses above. A device's state
   layer takes every block, then restores them all in one shot, and refuses a block offered after
   that restore -- and the restore happens early, long before a content store that is deliberately
   mounted after the radio. So the record is REGISTERED and FILLED before the consumer begins, and
   begin() must keep what it finds there.
 *
 * This is here because the relay could not follow the documented order and the warning that said so
 * was the only sign that the attempt counter -- the sole protection against an image that boots,
 * dies, and is applied again for ever -- had silently not been armed. Every other test in this file
 * loads the record AFTER begin, so none of them could have caught it. */
static void test_the_record_is_loaded_before_begin(void) {
    printf("\nthe durable record may be filled BEFORE begin, as a node's state layer requires\n");

    /* boot 1: make a real attempt, so g_durable holds a record worth preserving */
    rig(false, true);
    CHECK(bundle(0x053782u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied, restart asked for");
    iotdata_node_content_end(&g_store);
    CHECK(g_apply.persist.attempts == 1u, "one attempt recorded");

    /* boot 2, in the device's order: the block is addressed and filled, and only THEN does the
       consumer begin. Nothing here calls begin() first. */
    memset(&g_slot, 0, sizeof(g_slot));
    memset(&g_hash, 0, sizeof(g_hash));
    memset(&g_apply, 0, sizeof(g_apply)); /* a restarted process: the struct is bss */
    size_t size = 0u;
    void *const rec = iotdata_node_content_ota_apply_record(&g_apply, &size);
    CHECK(rec != NULL && size == sizeof(g_apply.persist), "the block can be addressed before begin");
    memcpy(rec, g_durable.blob, size); /* iotdata_node_state_load, which runs before the mount */
    CHECK(g_apply.persist.attempts == 1u, "and the restore lands in it");

    (void)filestore_ram_init(&g_fs);
    iotdata_node_content_init(&g_store, &g_fs.backend);
    CHECK(iotdata_node_content_ota_apply_begin(&g_apply, &g_store, &g_record_vt, &g_device, &g_slot_vt, &g_hash_vt, g_codec_vt, g_patch_vt, MY_STAMP, true), "the consumer registers");
    CHECK(g_apply.persist.attempts == 1u, "AND BEGIN DID NOT WIPE IT, which is the whole point");

    char line[160];
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "so the last boot can still be reported");
    CHECK(strstr(line, "did NOT run") != NULL, "truthfully");

    /* and the counter still counts from where it was, rather than starting again */
    CHECK(bundle(0x053782u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "the bundle is still there");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "tried again");
    CHECK(g_apply.persist.attempts == 2u, "two, not one -- the count was not lost");
    iotdata_node_content_end(&g_store);
}

static void test_booted_image_clears_only_on_confirmation(void) {
    printf("\nthe record is cleared by CONFIRMATION, and by nothing else\n");

    /* boot 1: apply a bundle whose stamp is what the NEXT build will have */
    rig(false, true);
    CHECK(bundle(0x053791u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied");
    iotdata_node_content_end(&g_store);

    /* boot 2: we ARE the new image now -- a different build, with that stamp compiled into it. */
    rig_as(true, true, "202610120000");
    char line[160];
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the record is there");
    CHECK(strstr(line, "BOOTED") != NULL, "and says the update booted");
    CHECK(strstr(line, "awaiting confirmation") != NULL, "but is not finished with");
    CHECK(g_apply.persist.attempts == 1u, "SO THE RECORD STAYS: an image that cleared it and then rolled back would be applied again");

    /* the bootloader confirms it, and only now is the attempt over */
    iotdata_node_content_ota_apply_confirmed(&g_apply);
    CHECK(g_apply.persist.attempts == 0u, "confirmation clears the COUNT");
    CHECK(g_apply.persist.id != 0u, "but NOT the identity: a later rollback has to know what it is undoing");
    CHECK(!iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "and there is nothing left to report");
    CHECK(g_apply.persist.attempts == 0u, "the COUNT is zero, which is what 'nothing in flight' means");
    CHECK(g_apply.persist.id != 0u, "but WHICH bundle we are running is remembered -- a rollback needs it");

    /* boot 3: a clean start, still the new build */
    rig_as(true, true, "202610120000");
    CHECK(!iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the cleared record persisted as cleared");
    iotdata_node_content_end(&g_store);
}

static void test_confirmation_of_someone_else(void) {
    printf("\na confirmation that is not the attempted image leaves the record alone\n");
    rig(false, true);
    CHECK(bundle(0x0537a1u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied");
    /* A cable flash lands a DIFFERENT build, which confirms itself on its first boot. That must not
       be read as the attempted update having succeeded -- the attempt genuinely never ran. */
    rig_as(true, true, "202610119999");
    iotdata_node_content_ota_apply_confirmed(&g_apply);
    CHECK(g_apply.persist.attempts == 1u, "the record is untouched");
    CHECK(strcmp(g_apply.persist.stamp, "202610120000") == 0, "and still names the image it was trying to run");
    iotdata_node_content_end(&g_store);
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_content_ota_apply: the OTA consumer\n");
    test_not_ours();
    test_applies();
    test_the_record_precedes_the_activate();
    test_slot_refusals();
    test_payload_hash_mismatch();
    test_claimed_but_refused();
    test_a_delta_is_patched_onto_the_running_image();
    test_a_delta_needs_a_patcher();
    test_a_seam_survives_its_own_begin();
    test_the_codec_is_drained_after_the_payload_runs_out();
    test_a_codec_we_cannot_decode_is_refused();
    test_deflate_is_inflated_into_the_slot();
    test_a_truncated_deflate_stream_is_caught();
    test_the_count_belongs_to_the_bundle();
    test_a_deliberate_rollback_is_not_a_retry();
    test_disabled();
    test_attempt_survives_and_is_judged();
    test_the_record_is_loaded_before_begin();
    test_booted_image_clears_only_on_confirmation();
    test_confirmation_of_someone_else();
    if (fails > 0) {
        printf("\n%d FAILED\n", fails);
        return 1;
    }
    printf("\nall ok\n");
    return 0;
}
