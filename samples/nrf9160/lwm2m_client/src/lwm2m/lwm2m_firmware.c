/*
 * Copyright (c) 2019 Foundries.io
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <flash.h>
#include <dfu/mcuboot.h>
#include <dfu/flash_img.h>
#include <logging/log_ctrl.h>
#include <misc/reboot.h>
#include <net/lwm2m.h>

#include "settings.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(app_lwm2m_firmware, CONFIG_APP_LOG_LEVEL);

/* values used below are auto-generated by pm_config.h */
#include "pm_config.h"
#define FLASH_AREA_IMAGE_PRIMARY	PM_APP_ID
#define FLASH_AREA_IMAGE_SECONDARY	PM_MCUBOOT_SECONDARY_ID
#define FLASH_BANK_SIZE			PM_MCUBOOT_SECONDARY_SIZE

#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT)
static u8_t firmware_buf[CONFIG_LWM2M_COAP_BLOCK_SIZE];
#endif

static struct flash_img_context dfu_ctx;

#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT)
static int firmware_update_cb(u16_t obj_inst_id)
{
	struct update_counter update_counter;
	int ret = 0;

	LOG_DBG("Executing firmware update");

	/* Bump update counter so it can be verified on the next reboot */
	ret = fota_update_counter_read(&update_counter);
	if (ret) {
		LOG_ERR("Failed read update counter");
		goto cleanup;
	}
	LOG_INF("Update Counter: current %d, update %d",
		update_counter.current, update_counter.update);
	ret = fota_update_counter_update(COUNTER_UPDATE,
					 update_counter.current + 1);
	if (ret) {
		LOG_ERR("Failed to update the update counter: %d", ret);
		goto cleanup;
	}

	boot_request_upgrade(false);

	LOG_INF("Rebooting device");
	LOG_PANIC();
	sys_reboot(0);

	return 0;

cleanup:
	return ret;
}

static void *firmware_get_buf(u16_t obj_inst_id, u16_t res_id,
			      u16_t res_inst_id, size_t *data_len)
{
	*data_len = sizeof(firmware_buf);
	return firmware_buf;
}

static int firmware_block_received_cb(u16_t obj_inst_id,
				      u16_t res_id, u16_t res_inst_id,
				      u8_t *data, u16_t data_len,
				      bool last_block, size_t total_size)
{
	static u8_t percent_downloaded;
	static u32_t bytes_downloaded;
	u8_t downloaded;
	int ret = 0;

	if (total_size > FLASH_BANK_SIZE) {
		LOG_ERR("Artifact file size too big (%d)", total_size);
		return -EINVAL;
	}

	if (!data_len) {
		LOG_ERR("Data len is zero, nothing to write.");
		return -EINVAL;
	}

	/* Erase bank 1 before starting the write process */
	if (bytes_downloaded == 0) {
		flash_img_init(&dfu_ctx);
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
		LOG_INF("Download firmware started, erasing progressively.");
#else
		LOG_INF("Download firmware started, erasing second bank");
		ret = boot_erase_img_bank(FLASH_AREA_IMAGE_SECONDARY);
		if (ret != 0) {
			LOG_ERR("Failed to erase flash bank 1");
			goto cleanup;
		}
#endif
	}

	bytes_downloaded += data_len;

	/* display a % downloaded, if it's different */
	if (total_size) {
		downloaded = bytes_downloaded * 100 / total_size;
	} else {
		/* Total size is empty when there is only one block */
		downloaded = 100;
	}

	if (downloaded > percent_downloaded) {
		percent_downloaded = downloaded;
		LOG_INF("%d%%", percent_downloaded);
	}

	ret = flash_img_buffered_write(&dfu_ctx, data, data_len, last_block);
	if (ret < 0) {
		LOG_ERR("Failed to write flash block");
		goto cleanup;
	}

	if (!last_block) {
		/* Keep going */
		return ret;
	}

	if (total_size && (bytes_downloaded != total_size)) {
		LOG_ERR("Early last block, downloaded %d, expecting %d",
			bytes_downloaded, total_size);
		ret = -EIO;
	}

cleanup:
	bytes_downloaded = 0;
	percent_downloaded = 0;

	return ret;
}
#endif

int lwm2m_init_firmware(void)
{
#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT)
	lwm2m_firmware_set_update_cb(firmware_update_cb);
	/* setup data buffer for block-wise transfer */
	lwm2m_engine_register_pre_write_callback("5/0/0", firmware_get_buf);
	lwm2m_firmware_set_write_cb(firmware_block_received_cb);
#endif

	return 0;
}

int lwm2m_init_image(void)
{
	int ret = 0;
	struct update_counter counter;
	bool image_ok;

	/* Update boot status and update counter */
	ret = fota_update_counter_read(&counter);
	if (ret) {
		LOG_ERR("Failed read update counter");
		return ret;
	}
	LOG_INF("Update Counter: current %d, update %d",
		counter.current, counter.update);
	image_ok = boot_is_img_confirmed();
	LOG_INF("Image is%s confirmed OK", image_ok ? "" : " not");
	if (!image_ok) {
		ret = boot_write_img_confirmed();
		if (ret) {
			LOG_ERR("Couldn't confirm this image: %d", ret);
			return ret;
		}
		LOG_INF("Marked image as OK");
#if !defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
		ret = boot_erase_img_bank(FLASH_AREA_IMAGE_SECONDARY);
		if (ret) {
			LOG_ERR("Flash area %d erase: error %d",
				FLASH_AREA_IMAGE_SECONDARY, ret);
			return ret;
		}
#endif

		LOG_DBG("Erased flash area %d", FLASH_AREA_IMAGE_SECONDARY);

		if (counter.update != -1) {
			ret = fota_update_counter_update(COUNTER_CURRENT,
						counter.update);
			if (ret) {
				LOG_ERR("Failed to update the update "
					"counter: %d", ret);
				return ret;
			}
			ret = fota_update_counter_read(&counter);
			if (ret) {
				LOG_ERR("Failed to read update counter: %d",
					ret);
				return ret;
			}
			LOG_INF("Update Counter updated");
		}
	}

	/* Check if a firmware update status needs to be reported */
	if (counter.update != -1 &&
			counter.current == counter.update) {
		/* Successful update */
		LOG_INF("Firmware updated successfully");
		lwm2m_engine_set_u8("5/0/5", RESULT_SUCCESS);
	} else if (counter.update > counter.current) {
		/* Failed update */
		LOG_INF("Firmware failed to be updated");
		lwm2m_engine_set_u8("5/0/5", RESULT_UPDATE_FAILED);
	}

	return ret;
}
