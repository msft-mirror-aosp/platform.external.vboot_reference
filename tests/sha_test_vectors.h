/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* FIPS 180-2 test vectors for SHA-1, SHA-256 and SHA-512 */

#ifndef VBOOT_REFERENCE_SHA_TEST_VECTORS_H_
#define VBOOT_REFERENCE_SHA_TEST_VECTORS_H_

#include "sha.h"

char *oneblock_msg = "abc";
char *multiblock_msg1 = "abcdbcdecdefdefgefghfghighijhijkijkl"
    "jklmklmnlmnomnopnopq";
char *multiblock_msg2= "abcdefghbcdefghicdefghijdefghijkefghi"
    "jklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnop"
    "qrsmnopqrstnopqrstu";
char *long_msg;

uint8_t sha1_results[][SHA1_DIGEST_SIZE] = {
  {
    0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,
    0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,
    0x9c,0xd0,0xd8,0x9d
  },
  {
    0x84,0x98,0x3e,0x44,0x1c,0x3b,0xd2,0x6e,
    0xba,0xae,0x4a,0xa1,0xf9,0x51,0x29,0xe5,
    0xe5,0x46,0x70,0xf1
  },
  {
    0x34,0xaa,0x97,0x3c,0xd4,0xc4,0xda,0xa4,
    0xf6,0x1e,0xeb,0x2b,0xdb,0xad,0x27,0x31,
    0x65,0x34,0x01,0x6f
  }
};

uint8_t sha256_results[][SHA256_DIGEST_SIZE] = {
  {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
    0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
    0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
  },
  {
    0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,
    0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
    0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,
    0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1
  },
  {
    0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92,
    0x81,0xa1,0xc7,0xe2,0x84,0xd7,0x3e,0x67,
    0xf1,0x80,0x9a,0x48,0xa4,0x97,0x20,0x0e,
    0x04,0x6d,0x39,0xcc,0xc7,0x11,0x2c,0xd0
  }
};

uint8_t sha512_results[][SHA512_DIGEST_SIZE] = {
  {
    0xdd,0xaf,0x35,0xa1,0x93,0x61,0x7a,0xba,
    0xcc,0x41,0x73,0x49,0xae,0x20,0x41,0x31,
    0x12,0xe6,0xfa,0x4e,0x89,0xa9,0x7e,0xa2,
    0x0a,0x9e,0xee,0xe6,0x4b,0x55,0xd3,0x9a,
    0x21,0x92,0x99,0x2a,0x27,0x4f,0xc1,0xa8,
    0x36,0xba,0x3c,0x23,0xa3,0xfe,0xeb,0xbd,
    0x45,0x4d,0x44,0x23,0x64,0x3c,0xe8,0x0e,
    0x2a,0x9a,0xc9,0x4f,0xa5,0x4c,0xa4,0x9f
  },
  {
    0x8e,0x95,0x9b,0x75,0xda,0xe3,0x13,0xda,
    0x8c,0xf4,0xf7,0x28,0x14,0xfc,0x14,0x3f,
    0x8f,0x77,0x79,0xc6,0xeb,0x9f,0x7f,0xa1,
    0x72,0x99,0xae,0xad,0xb6,0x88,0x90,0x18,
    0x50,0x1d,0x28,0x9e,0x49,0x00,0xf7,0xe4,
    0x33,0x1b,0x99,0xde,0xc4,0xb5,0x43,0x3a,
    0xc7,0xd3,0x29,0xee,0xb6,0xdd,0x26,0x54,
    0x5e,0x96,0xe5,0x5b,0x87,0x4b,0xe9,0x09
  },
  {
    0xe7,0x18,0x48,0x3d,0x0c,0xe7,0x69,0x64,
    0x4e,0x2e,0x42,0xc7,0xbc,0x15,0xb4,0x63,
    0x8e,0x1f,0x98,0xb1,0x3b,0x20,0x44,0x28,
    0x56,0x32,0xa8,0x03,0xaf,0xa9,0x73,0xeb,
    0xde,0x0f,0xf2,0x44,0x87,0x7e,0xa6,0x0a,
    0x4c,0xb0,0x43,0x2c,0xe5,0x77,0xc3,0x1b,
    0xeb,0x00,0x9c,0x5c,0x2c,0x49,0xaa,0x2e,
    0x4e,0xad,0xb2,0x17,0xad,0x8c,0xc0,0x9b
  }
};

#endif  /* VBOOT_REFERENCE_SHA_TEST_VECTORS_H_ */
