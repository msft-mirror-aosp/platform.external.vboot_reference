/* Copyright 2015 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Functions for updating the TPM state with the status of boot path.
 */

#include "2common.h"
#include "2sha.h"
#include "2sysincludes.h"
#include "2tpm_bootmode.h"

/*
 * Input digests for PCR extend.
 * These are calculated as:
 *    SHA1("|Developer_Mode||Recovery_Mode||Keyblock_Mode|").
 * Developer_Mode can be 0 or 1.
 * Recovery_Mode can be 0 or 1.
 * Keyblock flags are defined in 2struct.h and assumed always 0 in recovery mode
 * or 7 in non-recovery mode.
 *
 * We map them to Keyblock_Mode as follows:
 *   -----------------------------------------
 *   Keyblock Flags            | Keyblock Mode
 *   -----------------------------------------
 *   0 recovery mode           |     0
 *   7 Normal-signed firmware  |     1
 */

const uint8_t kBootStateSHA1Digests[][VB2_SHA1_DIGEST_SIZE] = {
	/* SHA1(0x00|0x00|0x01) */
	{0x25, 0x47, 0xcc, 0x73, 0x6e, 0x95, 0x1f, 0xa4, 0x91, 0x98, 0x53, 0xc4,
			0x3a, 0xe8, 0x90, 0x86, 0x1a, 0x3b, 0x32, 0x64},

	/* SHA1(0x01|0x00|0x01) */
	{0xc4, 0x2a, 0xc1, 0xc4, 0x6f, 0x1d, 0x4e, 0x21, 0x1c, 0x73, 0x5c, 0xc7,
			0xdf, 0xad, 0x4f, 0xf8, 0x39, 0x11, 0x10, 0xe9},

	/* SHA1(0x00|0x01|0x00) */
	{0x62, 0x57, 0x18, 0x91, 0x21, 0x5b, 0x4e, 0xfc, 0x1c, 0xea, 0xb7, 0x44,
			0xce, 0x59, 0xdd, 0x0b, 0x66, 0xea, 0x6f, 0x73},

	/* SHA1(0x01|0x01|0x00) */
	{0x47, 0xec, 0x8d, 0x98, 0x36, 0x64, 0x33, 0xdc, 0x00, 0x2e, 0x77, 0x21,
			0xc9, 0xe3, 0x7d, 0x50, 0x67, 0x54, 0x79, 0x37},
};

const uint8_t *vb2_get_boot_state_digest(struct vb2_context *ctx)
{
	int index = (ctx->flags & VB2_CONTEXT_RECOVERY_MODE ? 2 : 0) +
			(ctx->flags & VB2_CONTEXT_DEVELOPER_MODE ? 1 : 0);

	return kBootStateSHA1Digests[index];
}
