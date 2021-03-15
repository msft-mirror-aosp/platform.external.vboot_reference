/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Functions for loading a kernel from disk.
 * (Firmware portion)
 */

#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2packed_key.h"
#include "2rsa.h"
#include "2sha.h"
#include "2secdata.h"
#include "2sysincludes.h"
#include "cgptlib.h"
#include "cgptlib_internal.h"
#include "gpt_misc.h"
#include "load_kernel_fw.h"
#include "vb2_common.h"
#include "vboot_api.h"
#include "vboot_kernel.h"
#include "vboot_struct.h"

#define LOWEST_TPM_VERSION 0xffffffff

enum vb2_boot_mode {
	/* Normal boot: kernel must be verified. */
	VB2_BOOT_MODE_NORMAL = 0,

	/* Recovery boot, regardless of dev mode state. */
	VB2_BOOT_MODE_RECOVERY = 1,

	/* Developer boot: self-signed kernel okay. */
	VB2_BOOT_MODE_DEVELOPER = 2,
};

/**
 * Return the current boot mode (normal, recovery, or dev).
 *
 * @param ctx          Vboot context
 * @return Current boot mode (see vb2_boot_mode enum).
 */
static enum vb2_boot_mode get_boot_mode(struct vb2_context *ctx)
{
	if (ctx->flags & VB2_CONTEXT_RECOVERY_MODE)
		return VB2_BOOT_MODE_RECOVERY;

	if (ctx->flags & VB2_CONTEXT_DEVELOPER_MODE)
		return VB2_BOOT_MODE_DEVELOPER;

	return VB2_BOOT_MODE_NORMAL;
}

/**
 * Check if a valid keyblock is required.
 *
 * @param ctx		Vboot context
 * @return 1 if valid keyblock required (officially signed kernel);
 *         0 if valid hash is enough (self-signed kernel).
 */
static int need_valid_keyblock(struct vb2_context *ctx)
{
	/* Normal and recovery modes always require official OS */
	if (get_boot_mode(ctx) != VB2_BOOT_MODE_DEVELOPER)
		return 1;

	/* FWMP can require developer mode to use signed kernels */
	if (vb2_secdata_fwmp_get_flag(
		ctx, VB2_SECDATA_FWMP_DEV_ENABLE_OFFICIAL_ONLY))
		return 1;

	/* Developers may require signed kernels */
	if (vb2_nv_get(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY))
		return 1;

	return 0;
}

/**
 * Return a pointer to the keyblock inside a vblock.
 *
 * Must only be called during or after vb2_verify_kernel_vblock().
 *
 * @param kbuf		Buffer containing vblock
 * @return The keyblock pointer.
 */
static struct vb2_keyblock *get_keyblock(uint8_t *kbuf)
{
	return (struct vb2_keyblock *)kbuf;
}

/**
 * Return a pointer to the kernel preamble inside a vblock.
 *
 * Must only be called during or after vb2_verify_kernel_vblock().
 *
 * @param kbuf		Buffer containing vblock
 * @return The kernel preamble pointer.
 */
static struct vb2_kernel_preamble *get_preamble(uint8_t *kbuf)
{
	return (struct vb2_kernel_preamble *)
			(kbuf + get_keyblock(kbuf)->keyblock_size);
}

/**
 * Return the offset of the kernel body from the start of the vblock.
 *
 * Must only be called during or after vb2_verify_kernel_vblock().
 *
 * @param kbuf		Buffer containing vblock
 * @return The offset of the kernel body from the vblock start, in bytes.
 */
static uint32_t get_body_offset(uint8_t *kbuf)
{
	return (get_keyblock(kbuf)->keyblock_size +
		get_preamble(kbuf)->preamble_size);
}

/**
 * Verify a kernel vblock.
 *
 * @param kbuf		Buffer containing the vblock
 * @param kbuf_size	Size of the buffer in bytes
 * @param kernel_subkey	Packed kernel subkey to use in validating keyblock
 * @param wb		Work buffer.  Must be at least
 *			VB2_VERIFY_KERNEL_PREAMBLE_WORKBUF_BYTES bytes.
 * @return VB2_SUCCESS, or non-zero error code.
 */
static vb2_error_t vb2_verify_kernel_vblock(
	struct vb2_context *ctx, uint8_t *kbuf, uint32_t kbuf_size,
	const struct vb2_packed_key *kernel_subkey,
	struct vb2_workbuf *wb)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	int need_keyblock_valid = need_valid_keyblock(ctx);
	int keyblock_valid = 1;  /* Assume valid */

	vb2_error_t rv;

	/* Unpack kernel subkey */
	struct vb2_public_key kernel_subkey2;
	if (VB2_SUCCESS != vb2_unpack_key(&kernel_subkey2, kernel_subkey)) {
		VB2_DEBUG("Unable to unpack kernel subkey\n");
		return VB2_ERROR_VBLOCK_KERNEL_SUBKEY;
	}

	if (vb2_hwcrypto_allowed(ctx))
		kernel_subkey2.allow_hwcrypto = 1;

	/*
	 * Clear any previous keyblock-valid flag (for example, from a previous
	 * kernel where the keyblock was signed but the preamble failed
	 * verification).
	 */
	sd->flags &= ~VB2_SD_FLAG_KERNEL_SIGNED;

	/* Verify the keyblock. */
	struct vb2_keyblock *keyblock = get_keyblock(kbuf);
	rv = vb2_verify_keyblock(keyblock, kbuf_size, &kernel_subkey2, wb);
	if (rv) {
		VB2_DEBUG("Verifying keyblock signature failed.\n");
		keyblock_valid = 0;

		/* Check if we must have an officially signed kernel */
		if (need_keyblock_valid) {
			VB2_DEBUG("Self-signed kernels not enabled.\n");
			return rv;
		}

		/* Otherwise, allow the kernel if the keyblock hash is valid */
		rv = vb2_verify_keyblock_hash(keyblock, kbuf_size, wb);
		if (rv) {
			VB2_DEBUG("Verifying keyblock hash failed.\n");
			return rv;
		}
	}

	/* Check the keyblock flags against boot flags. */
	if (!(keyblock->keyblock_flags &
	      ((ctx->flags & VB2_CONTEXT_DEVELOPER_MODE) ?
	       VB2_KEYBLOCK_FLAG_DEVELOPER_1 :
	       VB2_KEYBLOCK_FLAG_DEVELOPER_0))) {
		VB2_DEBUG("Keyblock developer flag mismatch.\n");
		keyblock_valid = 0;
		if (need_keyblock_valid)
			return VB2_ERROR_KERNEL_KEYBLOCK_DEV_FLAG;
	}
	if (!(keyblock->keyblock_flags &
	      ((ctx->flags & VB2_CONTEXT_RECOVERY_MODE) ?
	       VB2_KEYBLOCK_FLAG_RECOVERY_1 :
	       VB2_KEYBLOCK_FLAG_RECOVERY_0))) {
		VB2_DEBUG("Keyblock recovery flag mismatch.\n");
		keyblock_valid = 0;
		if (need_keyblock_valid)
			return VB2_ERROR_KERNEL_KEYBLOCK_REC_FLAG;
	}

	/* Check for rollback of key version except in recovery mode. */
	enum vb2_boot_mode boot_mode = get_boot_mode(ctx);
	uint32_t key_version = keyblock->data_key.key_version;
	if (boot_mode != VB2_BOOT_MODE_RECOVERY) {
		if (key_version < (sd->kernel_version_secdata >> 16)) {
			VB2_DEBUG("Key version too old.\n");
			keyblock_valid = 0;
			if (need_keyblock_valid)
				return VB2_ERROR_KERNEL_KEYBLOCK_VERSION_ROLLBACK;
		}
		if (key_version > VB2_MAX_KEY_VERSION) {
			/*
			 * Key version is stored in 16 bits in the TPM, so key
			 * versions greater than 0xFFFF can't be stored
			 * properly.
			 */
			VB2_DEBUG("Key version > 0xFFFF.\n");
			keyblock_valid = 0;
			if (need_keyblock_valid)
				return VB2_ERROR_KERNEL_KEYBLOCK_VERSION_RANGE;
		}
	}

	/* If in developer mode and using key hash, check it */
	if (boot_mode == VB2_BOOT_MODE_DEVELOPER &&
	    vb2_secdata_fwmp_get_flag(ctx, VB2_SECDATA_FWMP_DEV_USE_KEY_HASH)) {
		struct vb2_packed_key *key = &keyblock->data_key;
		uint8_t *buf = ((uint8_t *)key) + key->key_offset;
		uint32_t buflen = key->key_size;
		uint8_t digest[VB2_SHA256_DIGEST_SIZE];

		VB2_DEBUG("Checking developer key hash.\n");
		vb2_digest_buffer(buf, buflen, VB2_HASH_SHA256,
				  digest, sizeof(digest));

		uint8_t *fwmp_dev_key_hash =
			vb2_secdata_fwmp_get_dev_key_hash(ctx);
		if (fwmp_dev_key_hash == NULL) {
			VB2_DEBUG("Couldn't retrieve developer key hash.\n");
			return VB2_ERROR_VBLOCK_DEV_KEY_HASH;
		}

		if (0 != vb2_safe_memcmp(digest, fwmp_dev_key_hash,
					 VB2_SHA256_DIGEST_SIZE)) {
			int i;

			VB2_DEBUG("Wrong developer key hash.\n");
			VB2_DEBUG("Want: ");
			for (i = 0; i < VB2_SHA256_DIGEST_SIZE; i++)
				VB2_DEBUG("%02x", fwmp_dev_key_hash[i]);
			VB2_DEBUG("\nGot:  ");
			for (i = 0; i < VB2_SHA256_DIGEST_SIZE; i++)
				VB2_DEBUG("%02x", digest[i]);
			VB2_DEBUG("\n");

			return VB2_ERROR_VBLOCK_DEV_KEY_HASH;
		}
	}

	/*
	 * At this point, we've checked everything.  The kernel keyblock is at
	 * least self-consistent, and has either a valid signature or a valid
	 * hash.  Track if it had a valid signature (that is, would we have
	 * been willing to boot it even if developer mode was off).
	 */
	if (keyblock_valid)
		sd->flags |= VB2_SD_FLAG_KERNEL_SIGNED;

	/* Get key for preamble verification from the keyblock. */
	struct vb2_public_key data_key;
	rv = vb2_unpack_key(&data_key, &keyblock->data_key);
	if (rv) {
		VB2_DEBUG("Unable to unpack kernel data key\n");
		return rv;
	}

	/* Verify the preamble, which follows the keyblock */
	struct vb2_kernel_preamble *preamble = get_preamble(kbuf);
	rv = vb2_verify_kernel_preamble(preamble,
					kbuf_size - keyblock->keyblock_size,
					&data_key,
					wb);
	if (rv) {
		VB2_DEBUG("Preamble verification failed.\n");
		return rv;
	}

	/*
	 * Kernel preamble version is the lower 16 bits of the composite
	 * kernel version.
	 */
	if (preamble->kernel_version > VB2_MAX_PREAMBLE_VERSION)
		return VB2_ERROR_KERNEL_PREAMBLE_VERSION_RANGE;

	/* Combine with the key version. */
	sd->kernel_version = key_version << 16 | preamble->kernel_version;

	/* If not in recovery mode, check for rollback of the kernel version. */
	if (need_keyblock_valid &&
	    boot_mode != VB2_BOOT_MODE_RECOVERY &&
	    sd->kernel_version < sd->kernel_version_secdata) {
		VB2_DEBUG("Kernel version too low.\n");
		return VB2_ERROR_KERNEL_PREAMBLE_VERSION_ROLLBACK;
	}

	VB2_DEBUG("Kernel preamble is good.\n");
	return VB2_SUCCESS;
}

enum vb2_load_partition_flags {
	/* Only check the vblock to */
	VB2_LOAD_PARTITION_VBLOCK_ONLY = (1 << 0),
};

#define KBUF_SIZE 65536  /* Bytes to read at start of kernel partition */

/* Minimum context work buffer size needed for vb2_load_partition() */
#define VB2_LOAD_PARTITION_WORKBUF_BYTES	\
	(VB2_VERIFY_KERNEL_PREAMBLE_WORKBUF_BYTES + KBUF_SIZE)

/**
 * Load and verify a partition from the stream.
 *
 * @param ctx		Vboot context
 * @param stream	Stream to load kernel from
 * @param kernel_subkey	Key to use to verify vblock
 * @param flags		Flags (one or more of vb2_load_partition_flags)
 * @param params	Load-kernel parameters
 * @param wb            Workbuf for data storage
 * @return VB2_SUCCESS, or non-zero error code.
 */
static vb2_error_t vb2_load_partition(
	struct vb2_context *ctx, VbExStream_t stream,
	const struct vb2_packed_key *kernel_subkey, uint32_t flags,
	LoadKernelParams *params,
	struct vb2_workbuf *wb)
{
	uint32_t read_ms = 0, start_ts;
	struct vb2_workbuf wblocal = *wb;

	/* Allocate kernel header buffer in workbuf */
	uint8_t *kbuf = vb2_workbuf_alloc(&wblocal, KBUF_SIZE);
	if (!kbuf)
		return VB2_ERROR_LOAD_PARTITION_WORKBUF;

	start_ts = vb2ex_mtime();
	if (VbExStreamRead(stream, KBUF_SIZE, kbuf)) {
		VB2_DEBUG("Unable to read start of partition.\n");
		return VB2_ERROR_LOAD_PARTITION_READ_VBLOCK;
	}
	read_ms += vb2ex_mtime() - start_ts;

	if (VB2_SUCCESS !=
	    vb2_verify_kernel_vblock(ctx, kbuf, KBUF_SIZE, kernel_subkey,
				     &wblocal)) {
		return VB2_ERROR_LOAD_PARTITION_VERIFY_VBLOCK;
	}

	if (flags & VB2_LOAD_PARTITION_VBLOCK_ONLY)
		return VB2_SUCCESS;

	struct vb2_keyblock *keyblock = get_keyblock(kbuf);
	struct vb2_kernel_preamble *preamble = get_preamble(kbuf);

	/*
	 * Make sure the kernel starts at or before what we already read into
	 * kbuf.
	 *
	 * We could deal with a larger offset by reading and discarding the
	 * data in between the vblock and the kernel data.
	 */
	uint32_t body_offset = get_body_offset(kbuf);
	if (body_offset > KBUF_SIZE) {
		VB2_DEBUG("Kernel body offset is %u > 64KB.\n", body_offset);
		return VB2_ERROR_LOAD_PARTITION_BODY_OFFSET;
	}

	uint8_t *kernbuf = params->kernel_buffer;
	uint32_t kernbuf_size = params->kernel_buffer_size;
	if (!kernbuf) {
		/* Get kernel load address and size from the header. */
		kernbuf = (uint8_t *)((long)preamble->body_load_address);
		kernbuf_size = preamble->body_signature.data_size;
	} else if (preamble->body_signature.data_size > kernbuf_size) {
		VB2_DEBUG("Kernel body doesn't fit in memory.\n");
		return 	VB2_ERROR_LOAD_PARTITION_BODY_SIZE;
	}

	uint32_t body_toread = preamble->body_signature.data_size;
	uint8_t *body_readptr = kernbuf;

	/*
	 * If we've already read part of the kernel, copy that to the beginning
	 * of the kernel buffer.
	 */
	uint32_t body_copied = KBUF_SIZE - body_offset;
	if (body_copied > body_toread)
		body_copied = body_toread;  /* Don't over-copy tiny kernel */
	memcpy(body_readptr, kbuf + body_offset, body_copied);
	body_toread -= body_copied;
	body_readptr += body_copied;

	/* Read the kernel data */
	start_ts = vb2ex_mtime();
	if (body_toread && VbExStreamRead(stream, body_toread, body_readptr)) {
		VB2_DEBUG("Unable to read kernel data.\n");
		return VB2_ERROR_LOAD_PARTITION_READ_BODY;
	}
	read_ms += vb2ex_mtime() - start_ts;
	if (read_ms == 0)  /* Avoid division by 0 in speed calculation */
		read_ms = 1;
	VB2_DEBUG("read %u KB in %u ms at %u KB/s.\n",
		  (body_toread + KBUF_SIZE) / 1024, read_ms,
		  (uint32_t)(((body_toread + KBUF_SIZE) * VB2_MSEC_PER_SEC) /
			     (read_ms * 1024)));

	/* Get key for preamble/data verification from the keyblock. */
	struct vb2_public_key data_key;
	if (VB2_SUCCESS != vb2_unpack_key(&data_key, &keyblock->data_key)) {
		VB2_DEBUG("Unable to unpack kernel data key\n");
		return VB2_ERROR_LOAD_PARTITION_DATA_KEY;
	}

	if (vb2_hwcrypto_allowed(ctx))
		data_key.allow_hwcrypto = 1;

	/* Verify kernel data */
	if (VB2_SUCCESS != vb2_verify_data(kernbuf, kernbuf_size,
					   &preamble->body_signature,
					   &data_key, &wblocal)) {
		VB2_DEBUG("Kernel data verification failed.\n");
		return VB2_ERROR_LOAD_PARTITION_VERIFY_BODY;
	}

	/* If we're still here, the kernel is valid */
	VB2_DEBUG("Partition is good.\n");

	/* Save kernel data back to parameters */
	params->bootloader_address = preamble->bootloader_address;
	params->bootloader_size = preamble->bootloader_size;
	params->flags = vb2_kernel_get_flags(preamble);
	if (!params->kernel_buffer) {
		params->kernel_buffer = kernbuf;
		params->kernel_buffer_size = kernbuf_size;
	}

	return VB2_SUCCESS;
}

vb2_error_t LoadKernel(struct vb2_context *ctx, LoadKernelParams *params)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_workbuf wb;
	VbSharedDataKernelCall shcall;
	int found_partitions = 0;
	uint32_t lowest_version = LOWEST_TPM_VERSION;
	vb2_error_t rv;

	vb2_workbuf_from_ctx(ctx, &wb);

	/* Clear output params in case we fail */
	params->partition_number = 0;
	params->bootloader_address = 0;
	params->bootloader_size = 0;
	params->flags = 0;

	/*
	 * Set up tracking for this call.  This wraps around if called many
	 * times, so we need to initialize the call entry each time.
	 */
	memset(&shcall, 0, sizeof(shcall));
	shcall.boot_flags = (uint32_t)params->boot_flags;
	shcall.sector_size = (uint32_t)params->bytes_per_lba;

	/* Locate key to verify kernel.  This will either be a recovery key, or
	   a kernel subkey passed from firmware verification. */
	struct vb2_packed_key *kernel_subkey =
		vb2_member_of(sd, sd->kernel_key_offset);

	/* Read GPT data */
	GptData gpt;
	gpt.sector_bytes = (uint32_t)params->bytes_per_lba;
	gpt.streaming_drive_sectors = params->streaming_lba_count;
	gpt.gpt_drive_sectors = params->gpt_lba_count;
	gpt.flags = params->boot_flags & BOOT_FLAG_EXTERNAL_GPT
			? GPT_FLAG_EXTERNAL : 0;
	if (0 != AllocAndReadGptData(params->disk_handle, &gpt)) {
		VB2_DEBUG("Unable to read GPT data\n");
		goto gpt_done;
	}

	/* Initialize GPT library */
	if (GPT_SUCCESS != GptInit(&gpt)) {
		VB2_DEBUG("Error parsing GPT\n");
		goto gpt_done;
	}

	/* Loop over candidate kernel partitions */
	uint64_t part_start, part_size;
	while (GPT_SUCCESS ==
	       GptNextKernelEntry(&gpt, &part_start, &part_size)) {

		VB2_DEBUG("Found kernel entry at %"
			  PRIu64 " size %" PRIu64 "\n",
			  part_start, part_size);

		/* Found at least one kernel partition. */
		shcall.kernel_parts_found++;
		found_partitions++;

		/* Set up the stream */
		VbExStream_t stream = NULL;
		if (VbExStreamOpen(params->disk_handle,
				   part_start, part_size, &stream)) {
			VB2_DEBUG("Partition error getting stream.\n");
			VB2_DEBUG("Marking kernel as invalid.\n");
			GptUpdateKernelEntry(&gpt, GPT_UPDATE_ENTRY_BAD);
			continue;
		}

		uint32_t lpflags = 0;
		if (params->partition_number > 0) {
			/*
			 * If we already have a good kernel, we only needed to
			 * look at the vblock versions to check for rollback.
			 */
			lpflags |= VB2_LOAD_PARTITION_VBLOCK_ONLY;
		}

		rv = vb2_load_partition(ctx,
					stream,
					kernel_subkey,
					lpflags,
					params,
					&wb);
		VbExStreamClose(stream);

		if (rv != VB2_SUCCESS) {
			VB2_DEBUG("Marking kernel as invalid.\n");
			GptUpdateKernelEntry(&gpt, GPT_UPDATE_ENTRY_BAD);
			continue;
		}

		int keyblock_valid = sd->flags & VB2_SD_FLAG_KERNEL_SIGNED;
		/* Track lowest version from a valid header. */
		if (keyblock_valid && lowest_version > sd->kernel_version) {
			lowest_version = sd->kernel_version;
		}
		VB2_DEBUG("Keyblock valid: %d\n", keyblock_valid);
		VB2_DEBUG("Combined version: %u\n", sd->kernel_version);

		/*
		 * If we're only looking at headers, we're done with this
		 * partition.
		 */
		if (lpflags & VB2_LOAD_PARTITION_VBLOCK_ONLY)
			continue;

		/*
		 * Otherwise, we found a partition we like.
		 *
		 * TODO: GPT partitions start at 1, but cgptlib starts them at
		 * 0.  Adjust here, until cgptlib is fixed.
		 */
		params->partition_number = gpt.current_kernel + 1;

		/*
		 * TODO: GetCurrentKernelUniqueGuid() should take a destination
		 * size, or the dest should be a struct, so we know it's big
		 * enough.
		 */
		GetCurrentKernelUniqueGuid(&gpt, &params->partition_guid);

		/* Update GPT to note this is the kernel we're trying.
		 * But not when we assume that the boot process may
		 * not complete for valid reasons (eg. early shutdown).
		 */
		if (!(ctx->flags & VB2_CONTEXT_NOFAIL_BOOT))
			GptUpdateKernelEntry(&gpt, GPT_UPDATE_ENTRY_TRY);

		/*
		 * If we're in recovery mode or we're about to boot a
		 * non-officially-signed kernel, there's no rollback
		 * protection, so we can stop at the first valid kernel.
		 */
		if (get_boot_mode(ctx) == VB2_BOOT_MODE_RECOVERY ||
		    !keyblock_valid) {
			VB2_DEBUG("In recovery mode or dev-signed kernel\n");
			break;
		}

		/*
		 * Otherwise, we do care about the key index in the TPM.  If
		 * the good partition's key version is the same as the tpm,
		 * then the TPM doesn't need updating; we can stop now.
		 * Otherwise, we'll check all the other headers to see if they
		 * contain a newer key.
		 */
		if (sd->kernel_version == sd->kernel_version_secdata) {
			VB2_DEBUG("Same kernel version\n");
			break;
		}
	} /* while(GptNextKernelEntry) */

 gpt_done:
	/* Write and free GPT data */
	WriteAndFreeGptData(params->disk_handle, &gpt);

	/* Handle finding a good partition */
	if (params->partition_number > 0) {
		VB2_DEBUG("Good partition %d\n", params->partition_number);
		/*
		 * Validity check - only store a new TPM version if we found
		 * one. If lowest_version is still at its initial value, we
		 * didn't find one; for example, we're in developer mode and
		 * just didn't look.
		 */
		if (lowest_version != LOWEST_TPM_VERSION &&
		    lowest_version > sd->kernel_version_secdata)
			sd->kernel_version = lowest_version;

		/* Success! */
		rv = VB2_SUCCESS;
	} else if (found_partitions > 0) {
		rv = VB2_ERROR_LK_INVALID_KERNEL_FOUND;
	} else {
		rv = VB2_ERROR_LK_NO_KERNEL_FOUND;
	}

	shcall.return_code = (uint8_t)rv;
	return rv;
}
