/* Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Utility functions for Intel Flash Descriptor (ifd) and the 'Converged
 * Security and Manageability Engine' (CSME).
 */
#ifndef VBOOT_REFERENCE_FUTILITY_PLATFORM_CSME_H_
#define VBOOT_REFERENCE_FUTILITY_PLATFORM_CSME_H_

#include <stdint.h>
#include "updater_utils.h"

bool is_flash_descriptor_locked(const struct firmware_image *image);

#endif  /* VBOOT_REFERENCE_FUTILITY_PLATFORM_CSME_H_ */