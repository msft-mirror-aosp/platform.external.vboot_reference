/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * A reference implementation for AP (and supporting images) firmware updater.
 */

#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "2rsa.h"
#include "crossystem.h"
#include "fmap.h"
#include "futility.h"
#include "host_misc.h"
#include "utility.h"
#include "util_misc.h"
#include "vb2_common.h"
#include "vb2_struct.h"

#define COMMAND_BUFFER_SIZE 256
#define RETURN_ON_FAILURE(x) do {int r = (x); if (r) return r;} while (0);
#define FLASHROM_OUTPUT_WP_PATTERN "write protect is "

/* FMAP section names. */
static const char * const FMAP_RO_FRID = "RO_FRID",
		  * const FMAP_RO_SECTION = "RO_SECTION",
		  * const FMAP_RO_GBB = "GBB",
		  * const FMAP_RO_VPD = "RO_VPD",
		  * const FMAP_RW_VPD = "RW_VPD",
		  * const FMAP_RW_VBLOCK_A = "VBLOCK_A",
		  * const FMAP_RW_SECTION_A = "RW_SECTION_A",
		  * const FMAP_RW_SECTION_B = "RW_SECTION_B",
		  * const FMAP_RW_FWID = "RW_FWID",
		  * const FMAP_RW_FWID_A = "RW_FWID_A",
		  * const FMAP_RW_FWID_B = "RW_FWID_B",
		  * const FMAP_RW_SHARED = "RW_SHARED",
		  * const FMAP_RW_NVRAM = "RW_NVRAM",
		  * const FMAP_RW_LEGACY = "RW_LEGACY";

/* System environment values. */
static const char * const FWACT_A = "A",
		  * const FWACT_B = "B",
		  * const FLASHROM_OUTPUT_WP_ENABLED =
			  FLASHROM_OUTPUT_WP_PATTERN "enabled",
		  * const FLASHROM_OUTPUT_WP_DISABLED =
			  FLASHROM_OUTPUT_WP_PATTERN "disabled";

/* flashrom programmers. */
static const char * const PROG_HOST = "host",
		  * const PROG_EMULATE = "dummy:emulate",
		  * const PROG_EC = "ec",
		  * const PROG_PD = "ec:dev=1";

enum wp_state {
	WP_DISABLED,
	WP_ENABLED,
};

enum target_type {
	TARGET_SELF,
	TARGET_UPDATE,
};

enum active_slot {
	SLOT_UNKNOWN = -1,
	SLOT_A = 0,
	SLOT_B,
};

enum flashrom_ops {
	FLASHROM_READ,
	FLASHROM_WRITE,
	FLASHROM_WP_STATUS,
};

struct firmware_image {
	const char *programmer;
	char *emulation;
	uint32_t size;
	uint8_t *data;
	char *file_name;
	char *ro_version, *rw_version_a, *rw_version_b;
	FmapHeader *fmap_header;
};

struct firmware_section {
	uint8_t *data;
	size_t size;
};

struct system_property {
	int (*getter)();
	int value;
	int initialized;
};

enum system_property_type {
	SYS_PROP_MAINFW_ACT,
	SYS_PROP_TPM_FWVER,
	SYS_PROP_FW_VBOOT2,
	SYS_PROP_WP_HW,
	SYS_PROP_WP_SW,
	SYS_PROP_MAX
};

struct updater_config {
	struct firmware_image image, image_current;
	struct firmware_image ec_image, pd_image;
	int try_update;
	int force_update;
	int emulate;
	struct system_property system_properties[SYS_PROP_MAX];
};

/*
 * Strip a string (usually from shell execution output) by removing all the
 * trailing space characters (space, new line, tab, ... etc).
 */
static void strip(char *s)
{
	int len;
	assert(s);

	len = strlen(s);
	while (len-- > 0) {
		if (!isascii(s[len]) || !isspace(s[len]))
			break;
		s[len] = '\0';
	}
}

/*
 * Executes a command on current host and returns stripped command output.
 * If the command has failed (exit code is not zero), returns an empty string.
 * The caller is responsible for releasing the returned string.
 */
static char *host_shell(const char *command)
{
	/* Currently all commands we use do not have large output. */
	char buf[COMMAND_BUFFER_SIZE];

	int result;
	FILE *fp = popen(command, "r");

	Debug("%s: %s\n", __FUNCTION__, command);
	buf[0] = '\0';
	if (!fp) {
		Debug("%s: Execution error for %s.\n", __FUNCTION__, command);
		return strdup(buf);
	}

	if (fgets(buf, sizeof(buf), fp))
		strip(buf);
	result = pclose(fp);
	if (!WIFEXITED(result) || WEXITSTATUS(result) != 0) {
		Debug("%s: Execution failure with exit code %d: %s\n",
		      __FUNCTION__, WEXITSTATUS(result), command);
		/*
		 * Discard all output if command failed, for example command
		 * syntax failure may lead to garbage in stdout.
		 */
		buf[0] = '\0';
	}
	return strdup(buf);
}


/* An helper function to return "mainfw_act" system property.  */
static int host_get_mainfw_act()
{
	char buf[VB_MAX_STRING_PROPERTY];

	if (!VbGetSystemPropertyString("mainfw_act", buf, sizeof(buf)))
		return SLOT_UNKNOWN;

	if (strcmp(buf, FWACT_A) == 0)
		return SLOT_A;
	else if (strcmp(buf, FWACT_B) == 0)
		return SLOT_B;

	return SLOT_UNKNOWN;
}

/* A helper function to return the "tpm_fwver" system property. */
static int host_get_tpm_fwver()
{
	return VbGetSystemPropertyInt("tpm_fwver");
}

/* A helper function to return the "hardware write protection" status. */
static int host_get_wp_hw()
{
	/* wpsw refers to write protection 'switch', not 'software'. */
	int v = VbGetSystemPropertyInt("wpsw_cur");

	/* wpsw_cur may be not available, especially in recovery mode. */
	if (v < 0)
		v = VbGetSystemPropertyInt("wpsw_boot");

	return v;
}

/* A helper function to return "fw_vboot2" system property. */
static int host_get_fw_vboot2()
{
	return VbGetSystemPropertyInt("fw_vboot2");
}

/*
 * A helper function to invoke flashrom(8) command.
 * Returns 0 if success, non-zero if error.
 */
static int host_flashrom(enum flashrom_ops op, const char *image_path,
			 const char *programmer, int verbose,
			 const char *section_name)
{
	char *command, *result;
	const char *op_cmd, *dash_i = "-i", *postfix = "", *ignore_lock = "";
	int r;

	if (debugging_enabled)
		verbose = 1;

	if (!verbose)
		postfix = " >/dev/null 2>&1";

	if (!section_name || !*section_name) {
		dash_i = "";
		section_name = "";
	}

	if (strncmp(programmer, PROG_EMULATE, strlen(PROG_EMULATE)) == 0) {
		ignore_lock = "--ignore-lock";
	}

	switch (op) {
	case FLASHROM_READ:
		op_cmd = "-r";
		assert(image_path);
		break;

	case FLASHROM_WRITE:
		op_cmd = "-w";
		assert(image_path);
		break;

	case FLASHROM_WP_STATUS:
		op_cmd = "--wp-status";
		assert(image_path == NULL);
		image_path = "";
		/* grep is needed because host_shell only returns 1 line. */
		postfix = " 2>/dev/null | grep \"" \
			   FLASHROM_OUTPUT_WP_PATTERN "\"";
		break;

	default:
		assert(0);
		return -1;
	}

	/* TODO(hungte) In future we should link with flashrom directly. */
	r = asprintf(&command, "flashrom %s %s -p %s %s %s %s %s", op_cmd,
		     image_path, programmer, dash_i, section_name, ignore_lock,
		     postfix);

	if (r == -1) {
		/* `command` will be not available. */
		Error("%s: Cannot allocate memory for command to execute.\n",
		      __FUNCTION__);
		return -1;
	}

	if (verbose)
		printf("Executing: %s\n", command);

	if (op != FLASHROM_WP_STATUS) {
		r = system(command);
		free(command);
		return r;
	}

	result = host_shell(command);
	strip(result);
	free(command);
	Debug("%s: wp-status: %s\n", __FUNCTION__, result);

	if (strstr(result, FLASHROM_OUTPUT_WP_ENABLED))
		r = WP_ENABLED;
	else if (strstr(result, FLASHROM_OUTPUT_WP_DISABLED))
		r = WP_DISABLED;
	else
		r = -1;
	free(result);
	return r;
}

/* Helper function to return software write protection switch status. */
static int host_get_wp_sw()
{
	return host_flashrom(FLASHROM_WP_STATUS, NULL, PROG_HOST, 0, NULL);
}

/*
 * Gets the system property by given type.
 * If the property was not loaded yet, invoke the property getter function
 * and cache the result.
 * Returns the property value.
 */
static int get_system_property(enum system_property_type property_type,
			       struct updater_config *cfg)
{
	struct system_property *prop;

	assert(property_type < SYS_PROP_MAX);
	prop = &cfg->system_properties[property_type];
	if (!prop->initialized) {
		prop->initialized = 1;
		prop->value = prop->getter();
	}
	return prop->value;
}

static void print_system_properties(struct updater_config *cfg)
{
	int i;

	/*
	 * There may be error messages when fetching properties from active
	 * system, so we want to peek at them first and then print out.
	 */
	Debug("Scanning system properties...\n");
	for (i = 0; i < SYS_PROP_MAX; i++) {
		get_system_property((enum system_property_type)i, cfg);
	}

	printf("System properties: [");
	for (i = 0; i < SYS_PROP_MAX; i++) {
		printf("%d,",
		       get_system_property((enum system_property_type)i, cfg));
	}
	printf("]\n");
}

/*
 * Overrides the return value of a system property.
 * After invoked, next call to get_system_property(type, cfg) will return
 * the given value.
 */
static void override_system_property(enum system_property_type property_type,
				     struct updater_config *cfg,
				     int value)
{
	struct system_property *prop;

	assert(property_type < SYS_PROP_MAX);
	prop = &cfg->system_properties[property_type];
	prop->initialized = 1;
	prop->value = value;
}

/*
 * Overrides system properties from a given list.
 * The list should be string of integers eliminated by comma and/or space.
 * For example, "1 2 3" and "1,2,3" both overrides first 3 properties.
 * To skip some properties you have to use comma, for example
 * "1, , 3" will only override the first and 3rd properties.
 * Invalid characters and fields will be ignored.
 *
 * The current implementation is only for unit testing.
 * In future we may extend this with name=value so users can use it easily on
 * actual systems.
 */
static void override_properties_from_list(const char *override_list,
					  struct updater_config *cfg)
{
	const char *s = override_list;
	char *e, c;
	int i = 0, wait_comma = 0;
	long int v;

	Debug("%s: Input is <%s>\n", __FUNCTION__, override_list);
	for (c = *s; c; c = *++s) {
		if (c == ',') {
			if (!wait_comma)
				i++;
			wait_comma = 0;
		}
		if (!isascii(c) || !isdigit(c))
			continue;
		if (i >= SYS_PROP_MAX) {
			Error("%s: Too many fields (max is %d): %s.\n",
			      __FUNCTION__, SYS_PROP_MAX, override_list);
			return;
		}
		v = strtol(s, &e, 0);
		s = e - 1;
		Debug("%s: property[%d].value = %d\n", __FUNCTION__, i, v);
		override_system_property((enum system_property_type)i, cfg, v);
		wait_comma = 1;
		i++;
	}
}

/*
 * Finds a firmware section by given name in the firmware image.
 * If successful, return zero and *section argument contains the address and
 * size of the section; otherwise failure.
 */
static int find_firmware_section(struct firmware_section *section,
				 const struct firmware_image *image,
				 const char *section_name)
{
	FmapAreaHeader *fah = NULL;
	uint8_t *ptr;

	section->data = NULL;
	section->size = 0;
	ptr = fmap_find_by_name(
			image->data, image->size, image->fmap_header,
			section_name, &fah);
	if (!ptr)
		return -1;
	section->data = (uint8_t *)ptr;
	section->size = fah->area_size;
	return 0;
}

/*
 * Returns true if the given FMAP section exists in the firmware image.
 */
static int firmware_section_exists(const struct firmware_image *image,
				   const char *section_name)
{
	struct firmware_section section;
	find_firmware_section(&section, image, section_name);
	return section.data != NULL;
}

/*
 * Loads the firmware information from an FMAP section in loaded firmware image.
 * The section should only contain ASCIIZ string as firmware version.
 * If successful, the return value is zero and *version points to a newly
 * allocated string as firmware version (caller must free it); otherwise
 * failure.
 */
static int load_firmware_version(struct firmware_image *image,
				 const char *section_name,
				 char **version)
{
	struct firmware_section fwid;
	find_firmware_section(&fwid, image, section_name);
	if (fwid.size) {
		*version = strndup((const char*)fwid.data, fwid.size);
		return 0;
	}
	*version = strdup("");
	return -1;
}

/*
 * Loads a firmware image from file.
 * Returns 0 on success, otherwise failure.
 */
static int load_image(const char *file_name, struct firmware_image *image)
{
	Debug("%s: Load image file from %s...\n", __FUNCTION__, file_name);

	if (vb2_read_file(file_name, &image->data, &image->size) != VB2_SUCCESS)
	{
		Error("%s: Failed to load %s\n", __FUNCTION__, file_name);
		return -1;
	}

	Debug("%s: Image size: %d\n", __FUNCTION__, image->size);
	assert(image->data);
	image->file_name = strdup(file_name);

	image->fmap_header = fmap_find(image->data, image->size);
	if (!image->fmap_header) {
		Error("Invalid image file (missing FMAP): %s\n", file_name);
		return -1;
	}

	if (!firmware_section_exists(image, FMAP_RO_FRID)) {
		Error("Does not look like VBoot firmware image: %s", file_name);
		return -1;
	}

	load_firmware_version(image, FMAP_RO_FRID, &image->ro_version);
	if (firmware_section_exists(image, FMAP_RW_FWID_A)) {
		char **a = &image->rw_version_a, **b = &image->rw_version_b;
		load_firmware_version(image, FMAP_RW_FWID_A, a);
		load_firmware_version(image, FMAP_RW_FWID_B, b);
	} else if (firmware_section_exists(image, FMAP_RW_FWID)) {
		char **a = &image->rw_version_a, **b = &image->rw_version_b;
		load_firmware_version(image, FMAP_RW_FWID, a);
		load_firmware_version(image, FMAP_RW_FWID, b);
	} else {
		Error("Unsupported VBoot firmware (no RW ID): %s", file_name);
	}
	return 0;
}

/*
 * Loads and emulates system firmware by an image file.
 * This will set a emulation programmer in image->emulation so flashrom
 * can access the file as system firmware storage.
 * Returns 0 if success, non-zero if error.
 */
static int emulate_system_image(const char *file_name,
				struct firmware_image *image)
{
	if (load_image(file_name, image))
		return -1;

	if (asprintf(&image->emulation,
		     "%s=VARIABLE_SIZE,image=%s,size=%u",
		     PROG_EMULATE, file_name, image->size) < 0) {
		Error("%s: Failed to allocate buffer for programmer: %s.\n",
		      __FUNCTION__, file_name);
		return -1;
	}
	return 0;
}

/*
 * Loads the active system firmware image (usually from SPI flash chip).
 * Returns 0 if success, non-zero if error.
 */
static int load_system_image(struct updater_config *cfg,
			     struct firmware_image *image)
{
	/* TODO(hungte) replace by mkstemp */
	const char *tmp_file = "/tmp/.fwupdate.read";

	RETURN_ON_FAILURE(host_flashrom(
			FLASHROM_READ, tmp_file, image->programmer, 0, NULL));
	return load_image(tmp_file, image);
}

/*
 * Frees the allocated resource from a firmware image object.
 */
static void free_image(struct firmware_image *image)
{
	free(image->data);
	free(image->file_name);
	free(image->ro_version);
	free(image->rw_version_a);
	free(image->rw_version_b);
	free(image->emulation);
	memset(image, 0, sizeof(*image));
}

/*
 * Decides which target in RW firmware to manipulate.
 * The `target` argument specifies if we want to know "the section to be
 * update" (TARGET_UPDATE), or "the (active) section * to check" (TARGET_SELF).
 * Returns the section name if success, otherwise NULL.
 */
static const char *decide_rw_target(struct updater_config *cfg,
				    enum target_type target,
				    int is_vboot2)
{
	const char *a = FMAP_RW_SECTION_A, *b = FMAP_RW_SECTION_B;
	int slot = get_system_property(SYS_PROP_MAINFW_ACT, cfg);

	/* In vboot1, always update B and check content with A. */
	if (!is_vboot2)
		return target == TARGET_UPDATE ? b : a;

	switch (slot) {
	case SLOT_A:
		return target == TARGET_UPDATE ? b : a;

	case SLOT_B:
		return target == TARGET_UPDATE ? a : b;
	}

	return NULL;
}

/*
 * Sets any needed system properties to indicate system should try the new
 * firmware on next boot.
 * The `target` argument is an FMAP section name indicating which to try.
 * Returns 0 if success, non-zero if error.
 */
static int set_try_cookies(struct updater_config *cfg, const char *target,
			   int is_vboot2)
{
	int tries = 6;
	const char *slot;

	/* EC Software Sync needs few more reboots. */
	if (cfg->ec_image.data)
		tries += 2;

	/* Find new slot according to target (section) name. */
	if (strcmp(target, FMAP_RW_SECTION_A) == 0)
		slot = FWACT_A;
	else if (strcmp(target, FMAP_RW_SECTION_B) == 0)
		slot = FWACT_B;
	else {
		Error("%s: Unknown target: %s\n", __FUNCTION__, target);
		return -1;
	}

	if (cfg->emulate) {
		printf("(emulation) Setting try_next to %s, try_count to %d.\n",
		       slot, tries);
		return 0;
	}

	if (is_vboot2 && VbSetSystemPropertyString("fw_try_next", slot)) {
		Error("Failed to set fw_try_next to %s.\n", slot);
		return -1;
	}
	if (VbSetSystemPropertyInt("fw_try_count", tries)) {
		Error("Failed to set fw_try_count to %d.\n", tries);
		return -1;
	}
	return 0;
}

/*
 * Emulates writing to firmware.
 * Returns 0 if success, non-zero if error.
 */
static int emulate_write_firmware(const char *filename,
				  const struct firmware_image *image,
				  const char *section_name)
{
	struct firmware_image to_image = {0};
	struct firmware_section from, to;
	int errorcnt = 0;

	from.data = image->data;
	from.size = image->size;

	if (load_image(filename, &to_image)) {
		Error("%s: Cannot load image from %s.\n", __FUNCTION__,
		      filename);
		return -1;
	}

	if (section_name) {
		find_firmware_section(&from, image, section_name);
		if (!from.data) {
			Error("%s: No section %s in source image %s.\n",
			      __FUNCTION__, section_name, image->file_name);
			errorcnt++;
		}
		find_firmware_section(&to, &to_image, section_name);
		if (!to.data) {
			Error("%s: No section %s in destination image %s.\n",
			      __FUNCTION__, section_name, filename);
			errorcnt++;
		}
	} else if (image->size != to_image.size) {
		Error("%s: Image size is different (%s:%d != %s:%d)\n",
		      __FUNCTION__, image->file_name, image->size,
		      to_image.file_name, to_image.size);
		errorcnt++;
	} else {
		to.data = to_image.data;
		to.size = to_image.size;
	}

	if (!errorcnt) {
		size_t to_write = Min(to.size, from.size);

		assert(from.data && to.data);
		Debug("%s: Writing %d bytes\n", __FUNCTION__, to_write);
		memcpy(to.data, from.data, to_write);
	}

	if (!errorcnt && vb2_write_file(
			filename, to_image.data, to_image.size)) {
		Error("%s: Failed writing to file: %s\n", __FUNCTION__,
		      filename);
		errorcnt++;
	}

	free_image(&to_image);
	return errorcnt;
}

/*
 * Writes a section from given firmware image to system firmware.
 * If section_name is NULL, write whole image.
 * Returns 0 if success, non-zero if error.
 */
static int write_firmware(struct updater_config *cfg,
			  const struct firmware_image *image,
			  const char *section_name)
{
	/* TODO(hungte) replace by mkstemp */
	const char *tmp_file = "/tmp/.fwupdate.write";
	const char *programmer = cfg->emulate ? image->emulation :
			image->programmer;

	if (cfg->emulate) {
		printf("%s: (emulation) %s %s from %s to %s.\n",
		       __FUNCTION__,
		       image->emulation ? "Writing" : "Skipped writing",
		       section_name ? section_name : "whole image",
		       image->file_name, programmer);

		if (!image->emulation)
			return 0;

		/*
		 * TODO(hungte): Extract the real target from image->emulation,
		 * and allow to emulate writing with flashrom.
		 */
		return emulate_write_firmware(
				cfg->image_current.file_name, image,
				section_name);

	}
	if (vb2_write_file(tmp_file, image->data, image->size) != VB2_SUCCESS) {
		Error("%s: Cannot write temporary file for output: %s\n",
		      __FUNCTION__, tmp_file);
		return -1;
	}
	return host_flashrom(FLASHROM_WRITE, tmp_file, programmer, 1,
			     section_name);
}

/*
 * Write a section from given firmware image to system firmware if possible.
 * If section_name is NULL, write whole image.  If the image has no data or if
 * the section does not exist, ignore and return success.
 * Returns 0 if success, non-zero if error.
 */
static int write_optional_firmware(struct updater_config *cfg,
				   const struct firmware_image *image,
				   const char *section_name)
{
	if (!image->data) {
		Debug("%s: No data in <%s> image.\n", __FUNCTION__,
		      image->programmer);
		return 0;
	}
	if (section_name && !firmware_section_exists(image, section_name)) {
		Debug("%s: Image %s<%s> does not have section %s.\n",
		      __FUNCTION__, image->file_name, image->programmer,
		      section_name);
		return 0;
	}

	return write_firmware(cfg, image, section_name);
}

/* Preserves (copies) the given section (by name) from image_from to image_to.
 * The offset may be different, and the section data will be directly copied.
 * If the section does not exist on all images, return as failure.
 * If the source section is larger, contents on destination be truncated.
 * If the source section is smaller, the remaining area is not modified.
 * Returns 0 if success, non-zero if error.
 */
static int preserve_firmware_section(const struct firmware_image *image_from,
				     struct firmware_image *image_to,
				     const char *section_name)
{
	struct firmware_section from, to;

	find_firmware_section(&from, image_from, section_name);
	find_firmware_section(&to, image_to, section_name);
	if (!from.data || !to.data)
		return -1;
	if (from.size > to.size) {
		printf("WARNING: %s: Section %s is truncated after updated.\n",
		       __FUNCTION__, section_name);
	}
	/* Use memmove in case if we need to deal with sections that overlap. */
	memmove(to.data, from.data, Min(from.size, to.size));
	return 0;
}

/*
 * Finds the GBB (Google Binary Block) header on a given firmware image.
 * Returns a pointer to valid GBB header, or NULL on not found.
 */
static struct vb2_gbb_header *find_gbb(const struct firmware_image *image)
{
	struct firmware_section section;
	struct vb2_gbb_header *gbb_header;

	find_firmware_section(&section, image, FMAP_RO_GBB);
	gbb_header = (struct vb2_gbb_header *)section.data;
	/*
	 * futil_valid_gbb_header needs v1 header (GoogleBinaryBlockHeader)
	 * but that should be compatible with vb2_gbb_header
	 */
	if (!futil_valid_gbb_header((GoogleBinaryBlockHeader *)gbb_header,
				    section.size, NULL)) {
		Error("%s: Cannot find GBB in image: %s.\n", __FUNCTION__,
		      image->file_name);
		return NULL;
	}
	return gbb_header;
}

/*
 * Preserve the GBB contents from image_from to image_to.
 * Currently only GBB flags and HWID are preserved.
 * Returns 0 if success, otherwise -1 if GBB header can't be found or if HWID is
 * too large.
 */
static int preserve_gbb(const struct firmware_image *image_from,
			struct firmware_image *image_to)
{
	int len;
	uint8_t *hwid_to, *hwid_from;
	struct vb2_gbb_header *gbb_from, *gbb_to;

	gbb_from = find_gbb(image_from);
	gbb_to = find_gbb(image_to);

	if (!gbb_from || !gbb_to)
		return -1;

	/* Preserve flags. */
	gbb_to->flags = gbb_from->flags;
	hwid_to = (uint8_t *)gbb_to + gbb_to->hwid_offset;
	hwid_from = (uint8_t *)gbb_from + gbb_from->hwid_offset;

	/* Preserve HWID. */
	len = strlen((const char *)hwid_from);
	if (len >= gbb_to->hwid_size)
		return -1;

	/* Zero whole area so we won't have garbage after NUL. */
	memset(hwid_to, 0, gbb_to->hwid_size);
	memcpy(hwid_to, hwid_from, len);
	return 0;
}

/*
 * Preserves the critical sections from the current (active) firmware.
 * Currently only GBB, VPD (RO+RW) and NVRAM sections are preserved.
 * Returns 0 if success, non-zero if error.
 */
static int preserve_images(struct updater_config *cfg)
{
	int errcnt = 0;
	struct firmware_image *from = &cfg->image_current, *to = &cfg->image;
	errcnt += preserve_gbb(from, to);
	errcnt += preserve_firmware_section(from, to, FMAP_RO_VPD);
	errcnt += preserve_firmware_section(from, to, FMAP_RW_VPD);
	errcnt += preserve_firmware_section(from, to, FMAP_RW_NVRAM);
	return errcnt;
}

/*
 * Compares if two sections have same size and data.
 * Returns 0 if given sections are the same, otherwise non-zero.
 */
static int compare_section(const struct firmware_section *a,
			   const struct firmware_section *b)
{
	if (a->size != b->size)
		return a->size - b->size;
	return memcmp(a->data, b->data, a->size);
}

/*
 * Returns if the images are different (should be updated) in given section.
 * If the section contents are the same or if the section does not exist on both
 * images, return value is 0 (no need to update). Otherwise the return value is
 * non-zero, indicating an update should be performed.
 * If section_name is NULL, compare whole images.
 */
static int section_needs_update(const struct firmware_image *image_from,
				const struct firmware_image *image_to,
				const char *section_name)
{
	struct firmware_section from, to;

	if (!section_name) {
		if (image_from->size != image_to->size)
			return -1;
		return memcmp(image_from->data, image_to->data, image_to->size);
	}

	find_firmware_section(&from, image_from, section_name);
	find_firmware_section(&to, image_to, section_name);

	return compare_section(&from, &to);
}

/*
 * Returns true if the write protection is enabled on current system.
 */
static int is_write_protection_enabled(struct updater_config *cfg)
{
	/* Default to enabled. */
	int wp = get_system_property(SYS_PROP_WP_HW, cfg);
	if (wp == WP_DISABLED)
		return wp;
	/* For error or enabled, check WP SW. */
	wp = get_system_property(SYS_PROP_WP_SW, cfg);
	/* Consider all errors as enabled. */
	if (wp != WP_DISABLED)
		return WP_ENABLED;
	return wp;
}

/*
 * Checks if the given firmware images are compatible with current platform.
 * In current implementation (following Chrome OS style), we assume the platform
 * is identical to the name before a dot (.) in firmware version.
 * Returns 0 for success, otherwise failure.
 */
static int check_compatible_platform(struct updater_config *cfg)
{
	int len;
	struct firmware_image *image_from = &cfg->image_current,
			      *image_to = &cfg->image;
	const char *from_dot = strchr(image_from->ro_version, '.'),
	           *to_dot = strchr(image_to->ro_version, '.');

	if (!from_dot || !to_dot) {
		Debug("%s: Missing dot (from=%p, to=%p)\n", from_dot, to_dot);
		return -1;
	}
	len = from_dot - image_from->ro_version + 1;
	Debug("%s: Platform: %*.*s\n", __FUNCTION__, len, len,
	      image_from->ro_version);
	return strncmp(image_from->ro_version, image_to->ro_version, len);
}

/*
 * Returns a valid root key from GBB header, or NULL on failure.
 */
static const struct vb2_packed_key *get_rootkey(
		const struct vb2_gbb_header *gbb)
{
	struct vb2_packed_key *key = NULL;

	key = (struct vb2_packed_key *)((uint8_t *)gbb + gbb->rootkey_offset);
	if (!packed_key_looks_ok(key, gbb->rootkey_size)) {
		Error("%s: Invalid root key.\n", __FUNCTION__);
		return NULL;
	}
	return key;
}

/*
 * Returns a key block key from given image section, or NULL on failure.
 */
static const struct vb2_keyblock *get_keyblock(
		const struct firmware_image *image,
		const char *section_name)
{
	struct firmware_section section;

	find_firmware_section(&section, image, section_name);
	/* A keyblock must be followed by a vb2_fw_preamble. */
	if (section.size < sizeof(struct vb2_keyblock) +
	    sizeof(struct vb2_fw_preamble)) {
		Error("%s: Invalid section: %s\n", __FUNCTION__, section_name);
		return NULL;
	}
	return (const struct vb2_keyblock *)section.data;
}

/*
 * Duplicates a key block and returns the duplicated block.
 * The caller must free the returned key block after being used.
 */
static struct vb2_keyblock *dupe_keyblock(const struct vb2_keyblock *block)
{
	struct vb2_keyblock *new_block;

	new_block = (struct vb2_keyblock *)malloc(block->keyblock_size);
	assert(new_block);
	memcpy(new_block, block, block->keyblock_size);
	return new_block;
}

/*
 * Verifies if keyblock is signed with given key.
 * Returns 0 on success, otherwise failure.
 */
static int verify_keyblock(const struct vb2_keyblock *block,
			   const struct vb2_packed_key *sign_key) {
	int r;
	uint8_t workbuf[VB2_WORKBUF_RECOMMENDED_SIZE];
	struct vb2_workbuf wb;
	struct vb2_public_key key;
	struct vb2_keyblock *new_block;

	if (block->keyblock_signature.sig_size == 0) {
		Error("%s: Keyblock is not signed.\n", __FUNCTION__);
		return -1;
	}
	vb2_workbuf_init(&wb, workbuf, sizeof(workbuf));
	if (VB2_SUCCESS != vb2_unpack_key(&key, sign_key)) {
		Error("%s: Invalid signing key,\n", __FUNCTION__);
		return -1;
	}

	/*
	 * vb2_verify_keyblock will destroy the signature inside keyblock
	 * so we have to verify with a local copy.
	 */
	new_block = dupe_keyblock(block);
	r = vb2_verify_keyblock(new_block, new_block->keyblock_size, &key, &wb);
	free(new_block);

	if (r != VB2_SUCCESS) {
		Error("%s: Error verifying key block.\n", __FUNCTION__);
		return -1;
	}
	return 0;
}

/*
 * Gets the data key and firmware version from a section on firmware image.
 * The section should contain a vb2_keyblock and a vb2_fw_preamble immediately
 * after key block so we can decode and save the data key and firmware version
 * into argument `data_key_version` and `firmware_version`.
 * Returns 0 for success, otherwise failure.
 */
static int get_key_versions(const struct firmware_image *image,
			    const char *section_name,
			    unsigned int *data_key_version,
			    unsigned int *firmware_version)
{
	const struct vb2_keyblock *keyblock = get_keyblock(image, section_name);
	const struct vb2_fw_preamble *pre;

	if (!keyblock)
		return -1;
	*data_key_version = keyblock->data_key.key_version;
	pre = (struct vb2_fw_preamble *)((uint8_t*)keyblock +
					 keyblock->keyblock_size);
	*firmware_version = pre->firmware_version;
	Debug("%s: %s: data key version = %d, firmware version = %d\n",
	      __FUNCTION__, image->file_name, *data_key_version,
	      *firmware_version);
	return 0;
}

/*
 * Checks if the root key in ro_image can verify vblocks in rw_image.
 * Returns 0 for success, otherwise failure.
 */
static int check_compatible_root_key(const struct firmware_image *ro_image,
				     const struct firmware_image *rw_image)
{
	const struct vb2_gbb_header *gbb = find_gbb(ro_image);
	const struct vb2_packed_key *rootkey;
	const struct vb2_keyblock *keyblock;

	if (!gbb)
		return -1;

	rootkey = get_rootkey(gbb);
	if (!rootkey)
		return -1;

	/* Assume VBLOCK_A and VBLOCK_B are signed in same way. */
	keyblock = get_keyblock(rw_image, FMAP_RW_VBLOCK_A);
	if (!keyblock)
		return -1;

	if (verify_keyblock(keyblock, rootkey) != 0) {
		const struct vb2_gbb_header *gbb_rw = find_gbb(rw_image);
		const struct vb2_packed_key *rootkey_rw = NULL;
		int is_same_key = 0;
		/*
		 * Try harder to provide more info.
		 * packed_key_sha1_string uses static buffer so don't call
		 * it twice in args list of one expression.
		 */
		if (gbb_rw)
			rootkey_rw = get_rootkey(gbb_rw);
		if (rootkey_rw) {
			if (rootkey->key_offset == rootkey_rw->key_offset &&
			    rootkey->key_size == rootkey_rw->key_size &&
			    memcmp(rootkey, rootkey_rw, rootkey->key_size +
				   rootkey->key_offset) == 0)
				is_same_key = 1;
		}
		printf("Current (RO) image root key is %s, ",
		       packed_key_sha1_string(rootkey));
		if (is_same_key)
			printf("same with target (RW) image. "
			       "Maybe RW corrupted?\n");
		else
			printf("target (RW) image is signed with rootkey %s.\n",
			       rootkey_rw ? packed_key_sha1_string(rootkey_rw) :
			       "<invalid>");
		return -1;
	}
	return 0;
}

/*
 * Checks if the given firmware image is signed with a key that won't be
 * blocked by TPM's anti-rollback detection.
 * Returns 0 for success, otherwise failure.
 */
static int check_compatible_tpm_keys(struct updater_config *cfg,
				     const struct firmware_image *rw_image)
{
	unsigned int data_key_version = 0, firmware_version = 0,
		     tpm_data_key_version = 0, tpm_firmware_version = 0,
		     tpm_fwver = 0;

	tpm_fwver = get_system_property(SYS_PROP_TPM_FWVER, cfg);
	if (tpm_fwver <= 0) {
		Error("%s: Invalid tpm_fwver: %d.\n", __FUNCTION__, tpm_fwver);
		return -1;
	}

	tpm_data_key_version = tpm_fwver >> 16;
	tpm_firmware_version = tpm_fwver & 0xffff;
	Debug("%s: TPM: data_key_version = %d, firmware_version = %d\n",
	      __FUNCTION__, tpm_data_key_version, tpm_firmware_version);

	if (get_key_versions(rw_image, FMAP_RW_VBLOCK_A, &data_key_version,
			     &firmware_version) != 0)
		return -1;

	if (tpm_data_key_version > data_key_version) {
		Error("%s: Data key version rollback detected (%d->%d).\n",
		      __FUNCTION__, tpm_data_key_version, data_key_version);
		return -1;
	}
	if (tpm_firmware_version > firmware_version) {
		Error("%s: Firmware version rollback detected (%d->%d).\n",
		      __FUNCTION__, tpm_firmware_version, firmware_version);
		return -1;
	}
	return 0;
}

enum updater_error_codes {
	UPDATE_ERR_DONE,
	UPDATE_ERR_NEED_RO_UPDATE,
	UPDATE_ERR_NO_IMAGE,
	UPDATE_ERR_SYSTEM_IMAGE,
	UPDATE_ERR_INVALID_IMAGE,
	UPDATE_ERR_SET_COOKIES,
	UPDATE_ERR_WRITE_FIRMWARE,
	UPDATE_ERR_PLATFORM,
	UPDATE_ERR_TARGET,
	UPDATE_ERR_ROOT_KEY,
	UPDATE_ERR_TPM_ROLLBACK,
	UPDATE_ERR_UNKNOWN,
};

static const char * const updater_error_messages[] = {
	[UPDATE_ERR_DONE] = "Done (no error)",
	[UPDATE_ERR_NEED_RO_UPDATE] = "RO changed and no WP. Need full update.",
	[UPDATE_ERR_NO_IMAGE] = "No image to update; try specify with -i.",
	[UPDATE_ERR_SYSTEM_IMAGE] = "Cannot load system active firmware.",
	[UPDATE_ERR_INVALID_IMAGE] = "The given firmware image is not valid.",
	[UPDATE_ERR_SET_COOKIES] = "Failed writing system flags to try update.",
	[UPDATE_ERR_WRITE_FIRMWARE] = "Failed writing firmware.",
	[UPDATE_ERR_PLATFORM] = "Your system platform is not compatible.",
	[UPDATE_ERR_TARGET] = "No valid RW target to update. Abort.",
	[UPDATE_ERR_ROOT_KEY] = "RW not signed by same RO root key",
	[UPDATE_ERR_TPM_ROLLBACK] = "RW not usable due to TPM anti-rollback.",
	[UPDATE_ERR_UNKNOWN] = "Unknown error.",
};

/*
 * The main updater for "Try-RW update", to update only one RW section
 * and try if it can boot properly on reboot.
 * This was also known as --mode=autoupdate,--wp=1 in legacy updater.
 * Returns UPDATE_ERR_DONE if success, otherwise error.
 */
static enum updater_error_codes update_try_rw_firmware(
		struct updater_config *cfg,
		struct firmware_image *image_from,
		struct firmware_image *image_to,
		int wp_enabled)
{
	const char *target;
	int has_update = 1;
	int is_vboot2 = get_system_property(SYS_PROP_FW_VBOOT2, cfg);

	preserve_gbb(image_from, image_to);
	if (!wp_enabled && section_needs_update(
			image_from, image_to, FMAP_RO_SECTION))
		return UPDATE_ERR_NEED_RO_UPDATE;

	printf("Checking compatibility...\n");
	if (check_compatible_root_key(image_from, image_to))
		return UPDATE_ERR_ROOT_KEY;
	if (check_compatible_tpm_keys(cfg, image_to))
		return UPDATE_ERR_TPM_ROLLBACK;

	Debug("%s: Firmware %s vboot2.\n", __FUNCTION__,
	      is_vboot2 ?  "is" : "is NOT");
	target = decide_rw_target(cfg, TARGET_SELF, is_vboot2);
	if (target == NULL) {
		Error("TRY-RW update needs system to boot in RW firmware.\n");
		return UPDATE_ERR_TARGET;
	}

	printf("Checking %s contents...\n", target);
	if (!firmware_section_exists(image_to, target)) {
		Error("Cannot find section '%s' on firmware image: %s\n",
		      target, image_to->file_name);
		return UPDATE_ERR_INVALID_IMAGE;
	}
	if (!cfg->force_update)
		has_update = section_needs_update(image_from, image_to, target);

	if (has_update) {
		target = decide_rw_target(cfg, TARGET_UPDATE, is_vboot2);
		printf(">> TRY-RW UPDATE: Updating %s to try on reboot.\n",
		       target);

		if (write_firmware(cfg, image_to, target))
			return UPDATE_ERR_WRITE_FIRMWARE;
		if (set_try_cookies(cfg, target, is_vboot2))
			return UPDATE_ERR_SET_COOKIES;
	} else {
		/* Clear trial cookies for vboot1. */
		if (!is_vboot2 && !cfg->emulate)
			VbSetSystemPropertyInt("fwb_tries", 0);
	}

	/* TODO(hungte): Add optional checks here that may change has_update. */
	if (!has_update)
		printf(">> No need to update.\n");

	return UPDATE_ERR_DONE;
}

/*
 * The main updater for "RW update".
 * This was also known as --mode=recovery, --wp=1 in legacy updater.
 * Returns UPDATE_ERR_DONE if success, otherwise error.
 */
static enum updater_error_codes update_rw_firmrware(
		struct updater_config *cfg,
		struct firmware_image *image_from,
		struct firmware_image *image_to)
{
	printf(">> RW UPDATE: Updating RW sections (%s, %s, and %s).\n",
	       FMAP_RW_SECTION_A, FMAP_RW_SECTION_B, FMAP_RW_SHARED);

	printf("Checking compatibility...\n");
	if (check_compatible_root_key(image_from, image_to))
		return UPDATE_ERR_ROOT_KEY;
	if (check_compatible_tpm_keys(cfg, image_to))
		return UPDATE_ERR_TPM_ROLLBACK;
	/*
	 * TODO(hungte) Speed up by flashing multiple sections in one
	 * command, or provide diff file.
	 */
	if (write_firmware(cfg, image_to, FMAP_RW_SECTION_A) ||
	    write_firmware(cfg, image_to, FMAP_RW_SECTION_B) ||
	    write_firmware(cfg, image_to, FMAP_RW_SHARED) ||
	    write_optional_firmware(cfg, image_to, FMAP_RW_LEGACY))
		return UPDATE_ERR_WRITE_FIRMWARE;

	return UPDATE_ERR_DONE;
}

/*
 * The main updater for "Full update".
 * This was also known as "--mode=factory" or "--mode=recovery, --wp=0" in
 * legacy updater.
 * Returns UPDATE_ERR_DONE if success, otherwise error.
 */
static enum updater_error_codes update_whole_firmware(
		struct updater_config *cfg,
		struct firmware_image *image_to)
{
	printf(">> FULL UPDATE: Updating whole firmware image(s), RO+RW.\n");
	preserve_images(cfg);

	printf("Checking compatibility...\n");
	if (check_compatible_tpm_keys(cfg, image_to))
		return UPDATE_ERR_TPM_ROLLBACK;

	/* FMAP may be different so we should just update all. */
	if (write_firmware(cfg, image_to, NULL) ||
	    write_optional_firmware(cfg, &cfg->ec_image, NULL) ||
	    write_optional_firmware(cfg, &cfg->pd_image, NULL))
		return UPDATE_ERR_WRITE_FIRMWARE;

	return UPDATE_ERR_DONE;
}

/*
 * The main updater to update system firmware using the configuration parameter.
 * Returns UPDATE_ERR_DONE if success, otherwise failure.
 */
static enum updater_error_codes update_firmware(struct updater_config *cfg)
{
	int wp_enabled;
	struct firmware_image *image_from = &cfg->image_current,
			      *image_to = &cfg->image;
	if (!image_to->data)
		return UPDATE_ERR_NO_IMAGE;

	printf(">> Target image: %s (RO:%s, RW/A:%s, RW/B:%s).\n",
	       image_to->file_name, image_to->ro_version,
	       image_to->rw_version_a, image_to->rw_version_b);

	if (!image_from->data) {
		/*
		 * TODO(hungte) Read only RO_SECTION, VBLOCK_A, VBLOCK_B,
		 * RO_VPD, RW_VPD, RW_NVRAM, RW_LEGACY.
		 */
		printf("Loading current system firmware...\n");
		if (load_system_image(cfg, image_from) != 0)
			return UPDATE_ERR_SYSTEM_IMAGE;
	}
	printf(">> Current system: %s (RO:%s, RW/A:%s, RW/B:%s).\n",
	       image_from->file_name, image_from->ro_version,
	       image_from->rw_version_a, image_from->rw_version_b);

	if (check_compatible_platform(cfg))
		return UPDATE_ERR_PLATFORM;

	wp_enabled = is_write_protection_enabled(cfg);
	printf(">> Write protection: %d (%s; HW=%d, SW=%d).\n", wp_enabled,
	       wp_enabled ? "enabled" : "disabled",
	       get_system_property(SYS_PROP_WP_HW, cfg),
	       get_system_property(SYS_PROP_WP_SW, cfg));

	if (debugging_enabled)
		print_system_properties(cfg);

	if (cfg->try_update) {
		enum updater_error_codes r;
		r = update_try_rw_firmware(cfg, image_from, image_to,
					   wp_enabled);
		if (r != UPDATE_ERR_NEED_RO_UPDATE)
			return r;
		printf("Warning: %s\n", updater_error_messages[r]);
	}

	if (wp_enabled)
		return update_rw_firmrware(cfg, image_from, image_to);
	else
		return update_whole_firmware(cfg, image_to);
}

/*
 * Releases all loaded images in an updater configuration object.
 */
static void unload_updater_config(struct updater_config *cfg)
{
	int i;
	for (i = 0; i < SYS_PROP_MAX; i++) {
		cfg->system_properties[i].initialized = 0;
		cfg->system_properties[i].value = 0;
	}
	free_image(&cfg->image);
	free_image(&cfg->image_current);
	free_image(&cfg->ec_image);
	free_image(&cfg->pd_image);
	cfg->emulate = 0;
}

/* Command line options */
static struct option const long_opts[] = {
	/* name  has_arg *flag val */
	{"image", 1, NULL, 'i'},
	{"ec_image", 1, NULL, 'e'},
	{"pd_image", 1, NULL, 'P'},
	{"try", 0, NULL, 't'},
	{"mode", 1, NULL, 'm'},
	{"force", 0, NULL, 'F'},
	{"wp", 1, NULL, 'W'},
	{"emulate", 1, NULL, 'E'},
	{"sys_props", 1, NULL, 'S'},
	{"debug", 0, NULL, 'd'},
	{"verbose", 0, NULL, 'v'},
	{"help", 0, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

static const char * const short_opts = "hi:e:tm:dv";

static void print_help(int argc, char *argv[])
{
	printf("\n"
		"Usage:  " MYNAME " %s [OPTIONS]\n"
		"\n"
		"-i, --image=FILE    \tAP (host) firmware image (image.bin)\n"
		"-e, --ec_image=FILE \tEC firmware image (i.e, ec.bin)\n"
		"    --pd_image=FILE \tPD firmware image (i.e, pd.bin)\n"
		"-t, --try           \tTry A/B update on reboot if possible\n"
		"\n"
		"Legacy and compatibility options:\n"
		"-m, --mode=MODE     \tRun updater in given mode\n"
		"    --force         \tForce update (skip checking contents)\n"
		"\n"
		"Debugging and testing options:\n"
		"    --wp=1|0        \tSpecify write protection status\n"
		"    --emulate=FILE  \tEmulate system firmware using file\n"
		"    --sys_props=LIST\tList of system properties to override\n"
		"-d, --debug         \tPrint debugging messages\n"
		"-v, --verbose       \tPrint verbose messages\n"
		"",
		argv[0]);
}

static int do_update(int argc, char *argv[])
{
	int i, r, errorcnt = 0;
	struct updater_config cfg = {
		.image = { .programmer = PROG_HOST, },
		.image_current = { .programmer = PROG_HOST, },
		.ec_image = { .programmer = PROG_EC, },
		.pd_image = { .programmer = PROG_PD, },
		.try_update = 0,
		.force_update = 0,
		.emulate = 0,
		.system_properties = {
			[SYS_PROP_MAINFW_ACT] = {.getter = host_get_mainfw_act},
			[SYS_PROP_TPM_FWVER] = {.getter = host_get_tpm_fwver},
			[SYS_PROP_FW_VBOOT2] = {.getter = host_get_fw_vboot2},
			[SYS_PROP_WP_HW] = {.getter = host_get_wp_hw},
			[SYS_PROP_WP_SW] = {.getter = host_get_wp_sw},
		},
	};

	printf(">> Firmware updater started.\n");

	opterr = 0;
	while ((i = getopt_long(argc, argv, short_opts, long_opts, 0)) != -1) {
		switch (i) {
		case 'i':
			errorcnt += load_image(optarg, &cfg.image);
			break;
		case 'e':
			errorcnt += load_image(optarg, &cfg.ec_image);
			break;
		case 'P':
			errorcnt += load_image(optarg, &cfg.pd_image);
			break;
		case 't':
			cfg.try_update = 1;
			break;
		case 'm':
			if (strcmp(optarg, "autoupdate") == 0) {
				cfg.try_update = 1;
			} else if (strcmp(optarg, "recovery") == 0) {
				cfg.try_update = 0;
			} else if (strcmp(optarg, "factory") == 0) {
				cfg.try_update = 0;
				if (!is_write_protection_enabled(&cfg)) {
					errorcnt++;
					Error("Mode %s needs WP disabled.\n",
					      optarg);
				}
			} else {
				errorcnt++;
				Error("Invalid mode: %s\n", optarg);
			}
			break;
		case 'W':
			r = strtol(optarg, NULL, 0);
			override_system_property(SYS_PROP_WP_HW, &cfg, r);
			override_system_property(SYS_PROP_WP_SW, &cfg, r);
			break;
		case 'E':
			cfg.emulate = 1;
			errorcnt += emulate_system_image(
					optarg, &cfg.image_current);
			/* Both image and image_current need emulation. */
			if (!errorcnt) {
				cfg.image.emulation = strdup(
						cfg.image_current.emulation);
			}
			break;
		case 'F':
			cfg.force_update = 1;
			break;
		case 'S':
			override_properties_from_list(optarg, &cfg);
			break;
		case 'v':
			/* TODO(hungte) Change to better verbosity control. */
			debugging_enabled = 1;
			break;
		case 'd':
			debugging_enabled = 1;
			break;

		case 'h':
			print_help(argc, argv);
			return !!errorcnt;
		case '?':
			errorcnt++;
			if (optopt)
				Error("Unrecognized option: -%c\n", optopt);
			else if (argv[optind - 1])
				Error("Unrecognized option (possibly '%s')\n",
				      argv[optind - 1]);
			else
				Error("Unrecognized option.\n");
			break;
		default:
			errorcnt++;
			Error("Failed parsing options.\n");
		}
	}
	if (optind < argc) {
		errorcnt++;
		Error("Unexpected arguments.\n");
	}
	if (!errorcnt) {
		int r = update_firmware(&cfg);
		if (r != UPDATE_ERR_DONE) {
			r = Min(r, UPDATE_ERR_UNKNOWN);
			Error("%s\n", updater_error_messages[r]);
			errorcnt++;
		}
	}
	printf(">> %s: Firmware updater %s.\n",
	       errorcnt ? "FAILED": "DONE",
	       errorcnt ? "stopped due to error" : "exited successfully");

	unload_updater_config(&cfg);
	return !!errorcnt;
}

DECLARE_FUTIL_COMMAND(update, do_update, VBOOT_VERSION_ALL,
		      "Update system firmware");
