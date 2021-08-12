/*
 * aw87xxx.c
 *
 * Copyright (c) 2020 AWINIC Technology CO., LTD
 *
 * Author: Bruce <zhaolei@awinic.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/irq.h>
#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/gameport.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include "aw87xxx_monitor.h"
#include "aw87xxx.h"
#include "aw_bin_parse.h"
#include "aw87339.h"
#include "aw87359.h"
#include "aw87369.h"
#include "aw87519.h"
#include "aw87559.h"

/*****************************************************************
* aw87xxx marco
******************************************************************/
#define AW87XXX_I2C_NAME		"aw87xxx_pa"
#define AW87XXX_DRIVER_VERSION		"v0.1.13.1"
#define AW87XXX_PRODUCT_NUM		4
#define AW87XXX_PRODUCT_NAME_LEN	8
#define AW87XXX_DEFAULT_REG_MUSIC	78
#define AW87XXX_DEFAULT_REG_OFF		2

/*************************************************************************
 * aw87xxx variable
 ************************************************************************/
static char aw87xxx_cfg_name[AW87XXX_MODE_MAX][AW87XXX_CFG_NAME_MAX] = {
	{"aw87xxx_pid_xx_off_"},
	{"aw87xxx_pid_xx_music_"},
	{"aw87xxx_pid_xx_voice_"},
	{"aw87xxx_pid_xx_fm_"},
	{"aw87xxx_pid_xx_rcv_"},
};

static char aw87xxx_support_product[][AW87XXX_PRODUCT_NAME_LEN] = {
	{"aw87339"},
	{"aw87359"},
	{"aw87369"},
	{"aw87519"},
	{"aw87559"},
};

static const unsigned char rawData_default_music[AW87XXX_DEFAULT_REG_MUSIC] = {
	0x78, 0x39, 0x79, 0xE5, 0x77, 0xD0, 0x77, 0xD0, 0x78, 0x7A, 0x79, 0x6C,
	0x77, 0x81, 0x66, 0x69, 0x58, 0xBC, 0x02, 0x49, 0x03, 0x0A, 0x04, 0x80,
	0x05, 0x08, 0x06, 0x10, 0x07, 0x83, 0x08, 0x4E, 0x09, 0x03, 0x0A, 0x08,
	0x0B, 0x4A, 0x0C, 0x03, 0x0D, 0x77, 0x0E, 0x7A, 0x0F, 0x51, 0x10, 0x58,
	0x60, 0x6A, 0x61, 0x12, 0x62, 0xB5, 0x63, 0x5A, 0x64, 0x55, 0x65, 0x57,
	0x67, 0x28, 0x68, 0x35, 0x69, 0x98, 0x70, 0x1C, 0x71, 0x9C, 0x72, 0x33,
	0x73, 0x40, 0x74, 0x0C, 0x01, 0x78
};
static const unsigned char rawData_default_off[AW87XXX_DEFAULT_REG_OFF] = { 0x01, 0x00 };

static LIST_HEAD(g_aw87xxx_device_list);
static DEFINE_MUTEX(g_aw87xxx_mutex_lock);
static unsigned int g_aw87xxx_dev_cnt;

#ifdef CONFIG_SND_SOC_AW87XXX
struct aw87xxx *g_aw87xxx = NULL;
#endif

static int aw87xxx_actflag_thread(void *pdata);
static void aw87xxx_kthread_toggle(struct aw87xxx *aw87xxx, bool flag);
/**********************************************************
* i2c write and read
**********************************************************/
static int aw87xxx_i2c_write(struct aw87xxx *aw87xxx,
			     unsigned char reg_addr, unsigned char reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_smbus_write_byte_data(aw87xxx->i2c_client,
						reg_addr, reg_data);
		if (ret < 0) {
			aw_dev_err(aw87xxx->dev,
				   "%s: i2c_write cnt=%d error=%d\n", __func__,
				   cnt, ret);
		} else {
			break;
		}
		cnt++;
		msleep(AW_I2C_RETRY_DELAY);
	}

	return ret;
}

static int aw87xxx_i2c_read(struct aw87xxx *aw87xxx,
			    unsigned char reg_addr, unsigned char *reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_smbus_read_byte_data(aw87xxx->i2c_client, reg_addr);
		if (ret < 0) {
			aw_dev_err(aw87xxx->dev,
				   "%s: i2c_read cnt=%d error=%d\n", __func__,
				   cnt, ret);
		} else {
			*reg_data = ret;
			break;
		}
		cnt++;
		msleep(AW_I2C_RETRY_DELAY);
	}

	return ret;
}

/************************************************************************
* aw87xxx hardware control
************************************************************************/
static void aw87xxx_hw_on(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);

	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 0);
		mdelay(2);
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 1);
		mdelay(2);
	}
	aw87xxx->hwen_flag = 1;

}

static void aw87xxx_hw_off(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);

	if (aw87xxx->product == AW87XXX_339 ||
	    aw87xxx->product == AW87XXX_359 ||
	    aw87xxx->product == AW87XXX_369) {
		aw_dev_info(aw87xxx->dev, "%s: aw873x9 hw always enabled!!!\n",
			    __func__);
		aw87xxx->hwen_flag = 1;
		return;
	}
	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 0);
		mdelay(2);
	}
	aw87xxx->hwen_flag = 0;

}

static void aw87xxx_hw_reset(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 0);
		mdelay(2);
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 1);
		mdelay(2);
	}
}

/*******************************************************************************
* aw87xxx control interface
******************************************************************************/
static int aw87xxx_audio_off(struct aw87xxx *aw87xxx)
{
	int i;
	struct aw87xxx_scene_param *scene_off =
	    &aw87xxx->aw87xxx_scene_ls.scene_off;

	aw87xxx->current_mode = AW87XXX_OFF_MODE;
	if (aw87xxx->hwen_flag) {
		mutex_lock(&aw87xxx->lock);
		if (scene_off->cfg_update_flag == AW87XXX_CFG_OK) {
			/*send firmware data */
			for (i = 0; i < scene_off->scene_cont->len; i = i + 2) {
				aw_dev_info(aw87xxx->dev,
					    "%s: reg=0x%02X, val=0x%02X\n",
					    __func__,
					    scene_off->scene_cont->data[i],
					    scene_off->scene_cont->data[i + 1]);
				aw87xxx_i2c_write(aw87xxx,
						  scene_off->scene_cont->data[i],
						  scene_off->scene_cont->data[i + 1]);
			}
		} else {
			aw_dev_err(aw87xxx->dev,
				   "%s: %s_i2c%d@0x%02X cfg error, and set default reg:%02x %02x\n", __func__,
				   aw87xxx_support_product[aw87xxx->product],
				   aw87xxx->i2c_seq, aw87xxx->i2c_addr, rawData_default_off[0], rawData_default_off[1]);
			aw87xxx_i2c_write(aw87xxx, rawData_default_off[0], rawData_default_off[1]);
		}
		mutex_unlock(&aw87xxx->lock);
	}
	aw87xxx_hw_off(aw87xxx);

	aw87xxx_monitor_stop(&aw87xxx->monitor);
	return 0;
}

static int aw87xxx_set_gain(struct aw87xxx *aw87xxx, bool is_gain_hold)
{
	uint8_t reg_data;
	uint8_t real_data;
	int ret;

	if (!aw87xxx->denoise_flag) {
		aw_dev_err(aw87xxx->dev, "%s denoise disable,have not gain_reg_addr\n",
			__func__);
		return -EFAULT;
	}

	ret = aw87xxx_i2c_read(aw87xxx, aw87xxx->gain_reg_addr, &real_data);
	if (ret < 0) {
		aw_dev_err(aw87xxx->dev, "%s read gain error \n", __func__);
		return ret;
	}
	aw_dev_info(aw87xxx->dev, "%s: read gain :0x%x \n", __func__, real_data);

	if ((!is_gain_hold) || (aw87xxx->current_mode == AW87XXX_OFF_MODE)) {
		reg_data = aw87xxx->raw_gain_array[aw87xxx->current_mode];
		aw87xxx->gain_hold_flag = 0;
	} else {
		reg_data = aw87xxx->actflag_gain_array[aw87xxx->current_mode];
		aw87xxx->gain_hold_flag = 1;
	}

	if (real_data == reg_data) {
		aw_dev_info(aw87xxx->dev, "%s gain no change \n", __func__);
		return 0;
	}

	aw_dev_info(aw87xxx->dev, "%s: set gain :0x%x \n", __func__, reg_data);

	ret = aw87xxx_i2c_write(aw87xxx, aw87xxx->gain_reg_addr, reg_data);
	if (ret < 0) {
		aw_dev_err(aw87xxx->dev, "%s write gain error \n", __func__);
		return ret;
	}

	return 0;
}


static int aw87xxx_scene_reg_update(struct aw87xxx *aw87xxx,
				    struct aw87xxx_scene_param *scene_param)
{
	struct aw87xxx_container *aw87xxx_cont = scene_param->scene_cont;
	int i;

	aw_dev_info(aw87xxx->dev, "%s enter, mode: %d\n", __func__,
		    scene_param->scene_mode);
	if (!aw87xxx->hwen_flag)
		aw87xxx_hw_on(aw87xxx);
	mutex_lock(&aw87xxx->lock);
	if (scene_param->cfg_update_flag == AW87XXX_CFG_OK) {
		/*send firmware data */
		for (i = 0; i < aw87xxx_cont->len; i = i + 2) {
			aw_dev_info(aw87xxx->dev,
				    "%s: reg=0x%02X, val = 0x%02X\n", __func__,
				    aw87xxx_cont->data[i],
				    aw87xxx_cont->data[i + 1]);

			if (aw87xxx_cont->data[i] == aw87xxx->gain_reg_addr &&
					aw87xxx->denoise_flag) {
				aw87xxx_set_gain(aw87xxx, aw87xxx->gain_hold_flag);
				continue;
			}

			aw87xxx_i2c_write(aw87xxx, aw87xxx_cont->data[i],
					  aw87xxx_cont->data[i + 1]);
		}
	} else {
		aw_dev_err(aw87xxx->dev, "%s: cfg file not ready, load defalut register.\n", __func__);
		for (i = 0; i < AW87XXX_DEFAULT_REG_MUSIC; i = i + 2) {
			aw_dev_info(aw87xxx->dev,
				    "%s: reg=0x%02X, val = 0x%02X\n", __func__,
				    rawData_default_music[i],
				    rawData_default_music[i + 1]);

			//if (rawData_default_music[i] == aw87xxx->gain_reg_addr &&
			//		aw87xxx->denoise_flag) {
			//	aw87xxx_set_gain(aw87xxx, aw87xxx->gain_hold_flag);
			//	continue;
			//}

			aw87xxx_i2c_write(aw87xxx, rawData_default_music[i],
					  rawData_default_music[i + 1]);
		}

	}
	mutex_unlock(&aw87xxx->lock);

	if ((aw87xxx->current_mode != AW87XXX_OFF_MODE)
	    && aw87xxx->actflag_auto_run && aw87xxx->denoise_flag)
		aw87xxx_kthread_toggle(aw87xxx, true);

	if (aw87xxx->current_mode == AW87XXX_RCV_MODE) {
		aw87xxx_monitor_stop(&aw87xxx->monitor);
	} else if (aw87xxx->monitor.monitor_flag
		   && (aw87xxx->monitor.cfg_update_flag == AW87XXX_CFG_OK)) {
		aw87xxx->monitor.pre_vmax = AW_VMAX_INIT_VAL;
		aw87xxx->monitor.first_entry = AW_FIRST_ENTRY;
		schedule_delayed_work(&aw87xxx->monitor.work,
					msecs_to_jiffies(3000));
	}

	return 0;
}

unsigned char aw87xxx_show_current_mode(int32_t channel)
{
	struct aw87xxx *aw87xxx;
	struct list_head *pos;
	uint8_t ret;

	list_for_each(pos, &g_aw87xxx_device_list) {
		aw87xxx = list_entry(pos, struct aw87xxx, list);
		if (aw87xxx == NULL) {
			aw_dev_err(aw87xxx->dev,
				   "%s: struct aw87xxx not ready\n", __func__);
			return -EPERM;
		}
		if (aw87xxx->pa_channel == channel) {
			aw_dev_info(aw87xxx->dev,
				    "%s: %s_i2c%d@0x%02X current_mode: %d, pa_channel: %d\n",
				    __func__,
				    aw87xxx_support_product[aw87xxx->product],
				    aw87xxx->i2c_seq,
				    aw87xxx->i2c_addr,
				    aw87xxx->current_mode, aw87xxx->pa_channel);
			ret = aw87xxx->current_mode;
		}
	}

	return ret;
}
EXPORT_SYMBOL(aw87xxx_show_current_mode);

int aw87xxx_audio_scene_load(uint8_t mode, int32_t channel)
{
	struct aw87xxx_scene_param *scene_param;
	struct list_head *pos;
	struct aw87xxx *aw87xxx;
	int ret;

	list_for_each(pos, &g_aw87xxx_device_list) {
		aw87xxx = list_entry(pos, struct aw87xxx, list);
		if (aw87xxx == NULL) {
			pr_err("%s: struct aw87xxx not ready!\n", __func__);
			return -EPERM;
		}
		if (aw87xxx->pa_channel == channel) {
			aw_dev_info(aw87xxx->dev,
				    "%s enter, mode=%d, pa_channel=%d\n",
				    __func__, mode, channel);
			switch (mode) {
			case AW87XXX_OFF_MODE:
				return aw87xxx_audio_off(aw87xxx);
			case AW87XXX_MUSIC_MODE:
				scene_param =
				    &aw87xxx->aw87xxx_scene_ls.scene_music;
				aw87xxx->current_mode = scene_param->scene_mode;
				ret = aw87xxx_scene_reg_update(aw87xxx,
							       scene_param);
				break;
			case AW87XXX_VOICE_MODE:
				scene_param =
				    &aw87xxx->aw87xxx_scene_ls.scene_voice;
				aw87xxx->current_mode = scene_param->scene_mode;
				ret = aw87xxx_scene_reg_update(aw87xxx,
							       scene_param);
				break;
			case AW87XXX_FM_MODE:
				scene_param =
				    &aw87xxx->aw87xxx_scene_ls.scene_fm;
				aw87xxx->current_mode = scene_param->scene_mode;
				ret = aw87xxx_scene_reg_update(aw87xxx,
							       scene_param);
				break;
			case AW87XXX_RCV_MODE:
				scene_param =
				    &aw87xxx->aw87xxx_scene_ls.scene_rcv;
				aw87xxx->current_mode = scene_param->scene_mode;
				ret = aw87xxx_scene_reg_update(aw87xxx,
							       scene_param);
				break;
			default:
				aw_dev_err(aw87xxx->dev,
					   "%s: unsupported mode: %d\n",
					   __func__, mode);
				return -EINVAL;
			}
		}
	}
	return ret;
}
EXPORT_SYMBOL(aw87xxx_audio_scene_load);

#ifndef AW87XXX_EVT_BUILD
/****************************************************************************
* aw873xx firmware cfg update
***************************************************************************/
static void aw87xxx_parse_chip_name(struct aw87xxx *aw87xxx,
				    uint32_t *chip_name, uint32_t len)
{
	int i;
	char chip_name_str[8] = { 0 };

	memcpy(chip_name_str, (char *)chip_name, len);
	for (i = 0; i < len; i++) {
		if (chip_name_str[i] == 'A')
			break;
	}
	memcpy(aw87xxx->chip_name, chip_name_str + i, len - i);
	aw_dev_info(aw87xxx->dev, "%s: chip name: %s\n",
		    __func__, aw87xxx->chip_name);
}

static int aw87xxx_get_parse_fw_way(struct aw87xxx *aw87xxx,
				    const struct firmware *cont)
{
	int i = 0;
	uint32_t sum_data = 0;
	uint32_t check_sum = 0;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	memcpy(&check_sum, cont->data, sizeof(check_sum));
	/*check bit */
	for (i = sizeof(check_sum); i < cont->size; i++)
		sum_data += cont->data[i];

	aw_dev_info(aw87xxx->dev, "%s: check_sum: %d sum_data: %d\n",
		    __func__, check_sum, sum_data);
	if (check_sum != sum_data) {
		aw_dev_info(aw87xxx->dev, "%s: parse non head firmware!\n",
			    __func__);
		return AW87XXX_OLD_FIRMWARE;
	} else {
		aw_dev_info(aw87xxx->dev, "%s: parse add head firmware!\n",
			    __func__);
		return AW87XXX_NEW_FIRMWARE;
	}
}

static int aw87xxx_parse_add_head_firmware(struct aw87xxx *aw87xxx,
					   struct aw87xxx_scene_param
					   *current_scene,
					   const struct firmware *cont)
{
	int i = 0;
	int ret = 0;
	struct ui_frame_header *frame_header;
	struct aw87xxx_container *curr_scene_cnt;

	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);
	if (current_scene->bin_file != NULL)
		current_scene->bin_file = NULL;

	current_scene->bin_file =
	    devm_kzalloc(aw87xxx->dev, sizeof(struct aw_bin) + cont->size, GFP_KERNEL);

	if (!current_scene->bin_file) {
		aw_dev_err(aw87xxx->dev, "%s: Error allocating memory\n",
			   __func__);
		return -ENOMEM;
	}

	memcpy(&current_scene->bin_file->info.data, cont->data, cont->size);
	for (i = 0; i < cont->size; i = i + 2) {
		aw_dev_info(aw87xxx->dev,
			    "%s; mode is %d; bin file addr:0x%02x, bin file data:0x%02x\n",
			    __func__, current_scene->scene_mode,
			    current_scene->bin_file->info.data[i],
			    current_scene->bin_file->info.data[i + 1]);
	}

	spin_lock(&aw87xxx->bin_parse_lock);
	ret = aw_parsing_bin_file(current_scene->bin_file);
	spin_unlock(&aw87xxx->bin_parse_lock);

	if (ret != 0) {
		aw_dev_err(aw87xxx->dev, "%s: File to parse bin file %d\n",
			   __func__, current_scene->scene_mode);
		return -EPERM;
	}

	if (current_scene->frame_header != NULL)
		current_scene->frame_header = NULL;

	frame_header = current_scene->frame_header =
		devm_kzalloc(aw87xxx->dev, sizeof(struct ui_frame_header), GFP_KERNEL);
	if (!frame_header) {
		aw_dev_err(aw87xxx->dev, "%s: Error allocating memory\n",
			   __func__);
		return -ENOMEM;
	}

	/*check addr wide, data wide and bin type */
	if (current_scene->bin_file->header_info[0].reg_byte_len !=
	    AW87XXX_ADDR_BYTE_LEN
	    || current_scene->bin_file->header_info[0].data_byte_len !=
	    AW87XXX_DATA_BYTE_LEN
	    || current_scene->bin_file->header_info[0].bin_data_type !=
	    AW87XXX_BIN_TYPE_REG) {
		aw_dev_err(aw87xxx->dev, "%s: %d bin file type mismatch\n",
			   __func__,
			   current_scene->bin_file->header_info[0].
			   bin_data_type);
	}

	aw_dev_info(aw87xxx->dev, "%s: bin_fh_ver:0x%08x\n",
		   __func__,
		   current_scene->bin_file->header_info[0].header_ver);
	aw_dev_info(aw87xxx->dev, "%s: ui_ver:0x%08x\n", __func__,
		   current_scene->bin_file->header_info[0].ui_ver);
	aw_dev_info(aw87xxx->dev, "%s: bin_data_ver:0x%08x\n", __func__,
		   current_scene->bin_file->header_info[0].bin_data_ver);
	aw_dev_info(aw87xxx->dev, "%s: reg_byte_len:0x%08x\n", __func__,
		    current_scene->bin_file->header_info[0].reg_byte_len);
	aw_dev_info(aw87xxx->dev, "%s: data_byte_len:0x%08x\n", __func__,
		    current_scene->bin_file->header_info[0].data_byte_len);
	aw_dev_info(aw87xxx->dev, "%s: bin_data_type:0x%08x\n", __func__,
		   current_scene->bin_file->header_info[0].bin_data_type);

	aw87xxx_parse_chip_name(aw87xxx,
				(uint32_t *) current_scene->bin_file->header_info[0].chip_type,
				sizeof(frame_header->chip_name));

	if (current_scene->scene_cont != NULL)
		current_scene->scene_cont = NULL;

	curr_scene_cnt = current_scene->scene_cont =
		devm_kzalloc(aw87xxx->dev, current_scene->bin_file->header_info[0].bin_data_len +
				sizeof(int), GFP_KERNEL);
	if (!curr_scene_cnt) {
		aw_dev_err(aw87xxx->dev, "%s: Error allocating memory\n",
			   __func__);
		devm_kfree(aw87xxx->dev, frame_header);
		return -ENOMEM;
	}
	curr_scene_cnt->len =
		current_scene->bin_file->header_info[0].valid_data_len;
	memcpy(curr_scene_cnt->data,
	       cont->data + current_scene->bin_file->header_info[0].valid_data_addr,
	       current_scene->bin_file->header_info[0].valid_data_len);

	current_scene->cfg_update_flag = AW87XXX_CFG_OK;

	for (i = 0; i < curr_scene_cnt->len; i = i + 2) {
		aw_dev_info(aw87xxx->dev,
				"%s; mode is %d; addr:0x%02x, data:0x%02x\n",
				__func__, current_scene->scene_mode,
				curr_scene_cnt->data[i],
				curr_scene_cnt->data[i + 1]);
		if (curr_scene_cnt->data[i] == aw87xxx->gain_reg_addr) {
			aw87xxx->raw_gain_array[current_scene->scene_mode] =
						curr_scene_cnt->data[i + 1];
			aw_dev_info(aw87xxx->dev,
					"%s: %s_i2c%d@0x%02X mode:%d, raw_gain:0x%02X\n",
					__func__,
					aw87xxx_support_product[aw87xxx->product],
					aw87xxx->i2c_seq, aw87xxx->i2c_addr,
					current_scene->scene_mode,
					curr_scene_cnt->data[i + 1]);
		}
	}

	aw_dev_info(aw87xxx->dev, "%s: %s update complete\n",
		    __func__, aw87xxx->cfg_name[current_scene->scene_mode]);
	return 0;
}

static void aw87xxx_parse_non_head_firmware(struct aw87xxx *aw87xxx,
					    struct aw87xxx_scene_param *current_scene,
					    const struct firmware *cont)
{
	int i = 0;
	struct aw87xxx_container *scene_cont = NULL;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	if (current_scene->scene_cont != NULL)
		current_scene->scene_cont = NULL;

	scene_cont = current_scene->scene_cont =
	    devm_kzalloc(aw87xxx->dev, cont->size + sizeof(int), GFP_KERNEL);
	if (!scene_cont) {
		aw_dev_err(aw87xxx->dev, "%s: error! allocating memory\n",
			   __func__);
		return;
	}

	scene_cont->len = cont->size;
	memcpy(scene_cont->data, cont->data, cont->size);
	current_scene->cfg_update_flag = AW87XXX_CFG_OK;

	for (i = 0; i < scene_cont->len; i = i + 2) {
		aw_dev_info(aw87xxx->dev, "%s: addr:0x%02X, data:0x%02X\n",
			    __func__, scene_cont->data[i],
			    scene_cont->data[i + 1]);
		if (scene_cont->data[i] == aw87xxx->gain_reg_addr) {
			aw87xxx->raw_gain_array[current_scene->scene_mode] =
			    scene_cont->data[i + 1];
			aw_dev_info(aw87xxx->dev,
				    "%s: %s_i2c%d@0x%02X mode:%d, raw_gain:0x%02x\n",
				    __func__,
				    aw87xxx_support_product[aw87xxx->product],
				    aw87xxx->i2c_seq, aw87xxx->i2c_addr,
				    current_scene->scene_mode,
				    scene_cont->data[i + 1]);
		}
	}
	aw_dev_info(aw87xxx->dev, "%s: %s update complete\n",
		    __func__, aw87xxx->cfg_name[current_scene->scene_mode]);
}

static void aw87xxx_parse_firmware(struct aw87xxx *aw87xxx,
				   struct aw87xxx_scene_param *current_scene,
				   const struct firmware *cont)
{
	int ret;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	ret = aw87xxx_get_parse_fw_way(aw87xxx, cont);
	if (ret == AW87XXX_OLD_FIRMWARE)
		aw87xxx_parse_non_head_firmware(aw87xxx, current_scene, cont);
	else if (ret == AW87XXX_NEW_FIRMWARE)
		aw87xxx_parse_add_head_firmware(aw87xxx, current_scene, cont);
	release_firmware(cont);
}

static void aw87xxx_scene_cfg_loaded(const struct firmware *cont, void *context)
{
	int ram_timer_val = 2000;
	struct aw87xxx_scene_param *current_scene = context;
	struct aw87xxx *aw87xxx = NULL;

	switch (current_scene->scene_mode) {
	case AW87XXX_OFF_MODE:
		aw87xxx = container_of(current_scene, struct aw87xxx,
				       aw87xxx_scene_ls.scene_off);
		break;
	case AW87XXX_MUSIC_MODE:
		aw87xxx = container_of(current_scene, struct aw87xxx,
				       aw87xxx_scene_ls.scene_music);
		break;
	case AW87XXX_VOICE_MODE:
		aw87xxx = container_of(current_scene, struct aw87xxx,
				       aw87xxx_scene_ls.scene_voice);
		break;
	case AW87XXX_FM_MODE:
		aw87xxx = container_of(current_scene, struct aw87xxx,
				       aw87xxx_scene_ls.scene_fm);
		break;
	case AW87XXX_RCV_MODE:
		aw87xxx = container_of(current_scene, struct aw87xxx,
				       aw87xxx_scene_ls.scene_rcv);
		break;
	default:
		pr_err("%s: unsupported scence: %d\n",
		       __func__, current_scene->scene_mode);
		return;
	}

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	current_scene->update_num++;
	if (!cont) {
		aw_dev_err(aw87xxx->dev, "%s: failed to read %s\n",
			   __func__,
			   aw87xxx->cfg_name[current_scene->scene_mode]);

		release_firmware(cont);
		if (current_scene->update_num < AW87XXX_LOAD_CFG_RETRIES) {
			aw_dev_info(aw87xxx->dev,
				    "%s:restart hrtimer to load firmware\n",
				    __func__);
			schedule_delayed_work(&aw87xxx->ram_work,
					      msecs_to_jiffies(ram_timer_val));
		}
		return;
	}

	aw_dev_info(aw87xxx->dev, "%s: loaded %s, size: %zu\n",
		    __func__, aw87xxx->cfg_name[current_scene->scene_mode],
		    cont ? cont->size : 0);
	/* aw87xxx ram update */
	aw87xxx_parse_firmware(aw87xxx, current_scene, cont);
}

static int aw87xxx_scene_update(struct aw87xxx *aw87xxx, uint8_t scence_mode)
{
	struct aw87xxx_scene_param *current_scene = NULL;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	switch (scence_mode) {
	case AW87XXX_OFF_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.scene_off;
		break;
	case AW87XXX_MUSIC_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.scene_music;
		break;
	case AW87XXX_VOICE_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.scene_voice;
		break;
	case AW87XXX_FM_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.scene_fm;
		break;
	case AW87XXX_RCV_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.scene_rcv;
		break;
	default:
		aw_dev_err(aw87xxx->dev, "%s: unsupported scence: %d\n",
			   __func__, scence_mode);
		return -EINVAL;
	}

	return request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
				       aw87xxx->cfg_name[scence_mode],
				       aw87xxx->dev, GFP_KERNEL,
				       current_scene, aw87xxx_scene_cfg_loaded);
}

static void aw87xxx_vmax_cfg_loaded(const struct firmware *cont, void *context)
{
	struct aw87xxx *aw87xxx = context;
	struct vmax_config *vmax_cfg = NULL;
	int vmax_cfg_num = 0, i;
	int ram_timer_val = 2000;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	aw87xxx->monitor.update_num++;
	if (!cont) {
		aw_dev_err(aw87xxx->dev, "%s: failed to read %s\n",
			   __func__, aw87xxx->vmax_cfg_name);
		release_firmware(cont);
		if (aw87xxx->monitor.update_num < AW87XXX_LOAD_CFG_RETRIES) {
			aw_dev_info(aw87xxx->dev,
				    "%s: restart hrtimer to load firmware\n",
				    __func__);
			schedule_delayed_work(&aw87xxx->ram_work,
					      msecs_to_jiffies(ram_timer_val));
		}
		return;
	}

	if (aw87xxx->monitor.vmax_cfg != NULL)
		aw87xxx->monitor.vmax_cfg = NULL;
	vmax_cfg_num = cont->size / sizeof(struct vmax_single_config);
	vmax_cfg = aw87xxx->monitor.vmax_cfg =
	    devm_kzalloc(aw87xxx->dev,
			 (vmax_cfg_num * sizeof(struct vmax_config) +
			  sizeof(vmax_cfg_num)), GFP_KERNEL);
	if (!vmax_cfg) {
		aw_dev_err(aw87xxx->dev, "%s: vmax_cfg devm_kzalloc failed\n",
			   __func__);
		return;
	}
	vmax_cfg->vmax_cfg_num = vmax_cfg_num;
	for (i = 0; i < cont->size; i += 8) {
		vmax_cfg->vmax_cfg_total[i / 8].min_thr =
		    (cont->data[i + 3] << 24) +
		    (cont->data[i + 2] << 16) +
		    (cont->data[i + 1] << 8) + (cont->data[i]);

		vmax_cfg->vmax_cfg_total[i / 8].vmax =
		    (cont->data[i + 7] << 24) + (cont->data[i + 6] << 16) +
		    (cont->data[i + 5] << 8) + (cont->data[i + 4]);

		aw_dev_info(aw87xxx->dev, "%s: minthr: %d ,vmax:0x%08x\n",
			    __func__, vmax_cfg->vmax_cfg_total[i / 8].min_thr,
			    vmax_cfg->vmax_cfg_total[i / 8].vmax);
	}
	release_firmware(cont);
	aw87xxx->monitor.cfg_update_flag = AW87XXX_CFG_OK;
	aw_dev_info(aw87xxx->dev, "%s: vbat monitor update complete\n",
		    __func__);
}

static int aw87xxx_vbat_monitor_update(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	return request_firmware_nowait(THIS_MODULE,
				       FW_ACTION_HOTPLUG,
				       aw87xxx->vmax_cfg_name,
				       aw87xxx->dev,
				       GFP_KERNEL,
				       aw87xxx, aw87xxx_vmax_cfg_loaded);
}

static void aw87xxx_cfg_work_routine(struct work_struct *work)
{
	struct aw87xxx *aw87xxx = container_of(work,
					       struct aw87xxx, ram_work.work);
	struct aw87xxx_scene_list *aw_scene_ls = &aw87xxx->aw87xxx_scene_ls;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	aw_scene_ls->scene_off.scene_mode = AW87XXX_OFF_MODE;
	aw_scene_ls->scene_music.scene_mode = AW87XXX_MUSIC_MODE;
	aw_scene_ls->scene_voice.scene_mode = AW87XXX_VOICE_MODE;
	aw_scene_ls->scene_fm.scene_mode = AW87XXX_FM_MODE;
	aw_scene_ls->scene_rcv.scene_mode = AW87XXX_RCV_MODE;

	if (aw_scene_ls->scene_off.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx,
				     aw_scene_ls->scene_off.scene_mode);
	if (aw_scene_ls->scene_music.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx,
				     aw_scene_ls->scene_music.scene_mode);
	if (aw_scene_ls->scene_voice.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx,
				     aw_scene_ls->scene_voice.scene_mode);
	if (aw_scene_ls->scene_fm.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx, aw_scene_ls->scene_fm.scene_mode);
	if (aw_scene_ls->scene_rcv.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx,
				     aw_scene_ls->scene_rcv.scene_mode);
	if (aw87xxx->monitor.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_vbat_monitor_update(aw87xxx);
}

static int aw87xxx_cfg_init(struct aw87xxx *aw87xxx)
{
#ifdef AWINIC_CFG_UPDATE_DELAY
	int cfg_timer_val = 8000;
#else
	int cfg_timer_val = 0;
#endif
	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	INIT_DELAYED_WORK(&aw87xxx->ram_work, aw87xxx_cfg_work_routine);
	schedule_delayed_work(&aw87xxx->ram_work,
			      msecs_to_jiffies(cfg_timer_val));
	return 0;
}
#endif
/****************************************************************************
* aw87xxx compatible
*****************************************************************************/
static int aw87xxx_product_select(struct aw87xxx *aw87xxx, uint8_t chip_type)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	switch (chip_type) {
	case AW87XXX_339:
		aw87xxx->product = AW87XXX_339;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87339_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87339_reg_access;
		break;
	case AW87XXX_359:
		aw87xxx->product = AW87XXX_359;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87359_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87359_reg_access;
		break;
	case AW87XXX_369:
		aw87xxx->product = AW87XXX_369;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87369_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87369_reg_access;
		break;
	case AW87XXX_519:
		aw87xxx->product = AW87XXX_519;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87519_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87519_reg_access;
		break;
	case AW87XXX_559:
		aw87xxx->product = AW87XXX_559;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87559_REG_MAX;
		aw87xxx->reg_param.reg_access = AW87559_reg_access;
		break;
	default:
		aw_dev_err(aw87xxx->dev, "%s: unverified chip: %d\n",
			   __func__, chip_type);
		return -EINVAL;
	}
	return 0;
}

static void aw87xxx_cfg_free(struct aw87xxx *aw87xxx)
{
	int i;
	struct aw87xxx_scene_param *scene_param =
	    &aw87xxx->aw87xxx_scene_ls.scene_off;

	mutex_lock(&aw87xxx->lock);
	for (i = 0; i < AW87XXX_MODE_MAX; i++) {
		if (scene_param->frame_header != NULL) {
			devm_kfree(aw87xxx->dev, scene_param->frame_header);
			scene_param->frame_header = NULL;
		}
		if (scene_param->scene_cont != NULL) {
			devm_kfree(aw87xxx->dev, scene_param->scene_cont);
			scene_param->scene_cont = NULL;
		}
		if (scene_param->bin_file != NULL) {
			devm_kfree(aw87xxx->dev, scene_param->bin_file);
			scene_param->bin_file = NULL;
		}
		aw_dev_info(aw87xxx->dev, "kfree %d cfg",
			    scene_param->scene_mode);
		scene_param++;
	}
	memset(&aw87xxx->aw87xxx_scene_ls, 0,
	       sizeof(struct aw87xxx_scene_list));
	if (aw87xxx->monitor.vmax_cfg != NULL) {
		devm_kfree(aw87xxx->dev, aw87xxx->monitor.vmax_cfg);
		aw87xxx->monitor.vmax_cfg = NULL;
	}
	aw87xxx->monitor.cfg_update_flag = AW87XXX_CFG_WAIT;
	aw87xxx->monitor.update_num = AW87XXX_CFG_NUM_INIT;
	mutex_unlock(&aw87xxx->lock);
}

static void aw87xxx_kthread_toggle(struct aw87xxx *aw87xxx, bool flag)
{
	struct task_struct *stop_task = NULL;

	if (flag) {
		aw_dev_info(aw87xxx->dev,
			    "%s enter, %s_i2c%d@0x%02X open actflag kthread!\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

		if (!aw87xxx->actflag_sync_task) {
			aw87xxx->actflag_sync_task =
			    kthread_run(aw87xxx_actflag_thread, (void *)aw87xxx,
					"%s", AW87XXX_I2C_NAME);
			if (IS_ERR(aw87xxx->actflag_sync_task))
				aw_dev_err(aw87xxx->dev,
					   "%s: can't create actflag thread!!!\n",
					   __func__);
			else
				aw_dev_info(aw87xxx->dev,
					    "%s: actflag thread created!\n",
					    __func__);
		} else {
			aw_dev_info(aw87xxx->dev,
				    "%s: actflag thread already running!!!\n",
				    __func__);
		}

	} else {
		aw_dev_info(aw87xxx->dev,
			    "%s enter, %s_i2c%d@0x%02X turn off kthread!\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
		if (aw87xxx->actflag_sync_task) {
			aw87xxx_i2c_write(aw87xxx, aw87xxx->gain_reg_addr,
					  aw87xxx->raw_gain_array[aw87xxx->current_mode]);
			aw_dev_info(aw87xxx->dev,"%s,enter gain_reg_addr=0x%0x,raw_gain_array=0x%0x\n",__func__,
					aw87xxx->gain_reg_addr, aw87xxx->raw_gain_array[aw87xxx->current_mode]);
			stop_task = aw87xxx->actflag_sync_task;
			aw87xxx->actflag_sync_task = NULL;
		}
		if (stop_task)
			kthread_stop(stop_task);
	}
}

static int aw87xxx_actflag_thread(void *pdata)
{
	struct aw87xxx *aw87xxx = pdata;
	int active_flag[2] = { 0 };
	int ret;
	uint32_t thread_off_cnt = 0;
	unsigned char err_read_flag = 0;

	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);
	while (!kthread_should_stop()) {
		if (aw87xxx->current_mode == AW87XXX_OFF_MODE
		    || !aw87xxx->hwen_flag) {
			thread_off_cnt++;
			aw_dev_err(aw87xxx->dev,
				   "%s: %s_i2c%d@0x%02X pause! current_mode:%d, hwen_flag:%d, cnt:%d\n",
				   __func__,
				   aw87xxx_support_product[aw87xxx->product],
				   aw87xxx->i2c_seq, aw87xxx->i2c_addr,
				   aw87xxx->current_mode,
				   aw87xxx->hwen_flag, thread_off_cnt);
		} else {
			thread_off_cnt = 0;
			mutex_lock(&aw87xxx->lock);
			ret = aw_check_dsp_ready();
			if (ret) {
				ret = aw_get_dsp_msg_data((char *)active_flag,
						  sizeof(active_flag),
						  INLINE_PARAM_ID_ACTIVE_FLAG,
						  aw87xxx->pa_channel);
				if (ret < 0) {
					pr_err
					    ("%s: failed to read active_flag, ret: %d\n",
					     __func__, ret);
					err_read_flag = 1;
				} else {
					aw_dev_info(aw87xxx->dev, "%s, get dsp msg: left_flag: %d, right_flag: %d\n",
						  __func__, active_flag[0], active_flag[1]);
					err_read_flag = 0;
				}
				if (!err_read_flag) {
					aw_dev_info(aw87xxx->dev,
						    "%s: %s_i2c%d@0x%02X, channel:%d, mode:%d, active_flag[0]:%d, raw_gain:0x%02X, actgain:0x%02X\n",
						    __func__,
						    aw87xxx_support_product[aw87xxx->product],
						    aw87xxx->i2c_seq, aw87xxx->i2c_addr,
						    aw87xxx->pa_channel,
						    aw87xxx->current_mode,
						    active_flag[0], 
						    aw87xxx->raw_gain_array[aw87xxx->current_mode],
						    aw87xxx->actflag_gain_array[aw87xxx->current_mode]);
					if (active_flag[0] && aw87xxx->denoise_flag)
						aw87xxx_set_gain(aw87xxx, false);
					else if (aw87xxx->denoise_flag)
						aw87xxx_set_gain(aw87xxx, true);
				}
			}
			mutex_unlock(&aw87xxx->lock);
		}
		msleep_interruptible(KTHREAD_SYNC_INTERVAL);
		if (thread_off_cnt >= 100) {
			aw_dev_info(aw87xxx->dev,
				    "%s: %s_i2c%d@0x%02X after 10s, thread down!!!\n",
				    __func__,
				    aw87xxx_support_product[aw87xxx->product],
				    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
			thread_off_cnt = 0;
			if (aw87xxx->denoise_flag)
				aw87xxx_kthread_toggle(aw87xxx, false);
		}
		if (kthread_should_stop()) {
			aw_dev_info(aw87xxx->dev, "%s: kthread stopped!!!\n", __func__);
			break;
		}
	}
	aw87xxx->gain_hold_flag = 0;
	return 0;
}

/****************************************************************************
* aw87xxx attribute
*****************************************************************************/
static ssize_t aw87xxx_get_reg(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i = 0;
	unsigned char reg_val = 0;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n",
		    __func__, aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	for (i = 0; i < aw87xxx->reg_param.reg_max; i++) {
		if (!(aw87xxx->reg_param.reg_access[i] & AW87XXX_REG_RD_ACCESS))
			continue;
		aw87xxx_i2c_read(aw87xxx, i, &reg_val);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"reg:0x%02X=0x%02X\n", i, reg_val);
	}
	return len;
}

static ssize_t aw87xxx_set_reg(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t len)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	unsigned int databuf[2] = { 0 };

	if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) == 2)
		aw87xxx_i2c_write(aw87xxx, databuf[0], databuf[1]);
	return len;
}

static ssize_t aw87xxx_get_hwen(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "hwen: %d\n",
			aw87xxx->hwen_flag);
	return len;
}

static ssize_t aw87xxx_set_hwen(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t ret;
	unsigned int state;

	ret = kstrtouint(buf, 10, &state);
	if (ret) {
		aw_dev_err(aw87xxx->dev, "%s: fail to change str to int\n",
			   __func__);
		return ret;
	}
	aw_dev_info(aw87xxx->dev, "%s: set hardware enable: %d\n", __func__,
		    state);
	if (state == 0)
		aw87xxx_hw_off(aw87xxx);
	else
		aw87xxx_hw_on(aw87xxx);
	return len;
}

static ssize_t aw87xxx_set_update(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t ret;
	unsigned int state;
	int cfg_timer_val = 0;
	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n",
			    __func__, aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	ret = kstrtouint(buf, 10, &state);
	if (ret) {
		aw_dev_err(aw87xxx->dev, "%s: fail to change str to int\n",
			   __func__);
		return ret;
	}
	if (state) {
		aw87xxx_cfg_free(aw87xxx);
		schedule_delayed_work(&aw87xxx->ram_work,
				      msecs_to_jiffies(cfg_timer_val));
		msleep(300);
	}
	return len;
}

static ssize_t aw87xxx_get_mode(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "0: off mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "1: music mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "2: voice mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "3: fm mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "4: rcv mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "current mode: %d\n",
			aw87xxx->current_mode);
	return len;
}

static ssize_t aw87xxx_set_mode(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	int ret;
	uint8_t mode;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n",
		    __func__, aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	ret = kstrtou8(buf, 0, &mode);
	if (ret) {
		aw_dev_err(aw87xxx->dev, "%s: invalid input to mode!\n",
			   __func__);
		return ret;
	}
	if (mode < 5) {
		aw_dev_info(aw87xxx->dev, "%s: %d\n", __func__, mode);
		aw87xxx_audio_scene_load(mode, aw87xxx->pa_channel);
	} else {
		aw_dev_err(aw87xxx->dev, "%s: input num out of range!\n",
			   __func__);
		return len;
	}
	return len;
}

static ssize_t aw87xxx_get_actflag(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t len = 0;
	int flag[2] = { 0 };
	int ret;

	ret = aw_get_dsp_msg_data((char *)flag, sizeof(flag),
				  INLINE_PARAM_ID_ACTIVE_FLAG,
				  aw87xxx->pa_channel);
	if (ret < 0)
		return ret;
	aw_dev_info(aw87xxx->dev,
		    "%s: %s_i2c%d@0x%02X left_flag: %d, right_flag: %d\n",
		    __func__, aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr, flag[0], flag[1]);
	len +=
	    snprintf(buf + len, PAGE_SIZE - len,
		     "%s: left_flag: %d, right_flag: %d\n",
		     aw87xxx_support_product[aw87xxx->product], flag[0],
		     flag[1]);
	return len;
}

static ssize_t aw87xxx_set_aftask(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	uint8_t val;
	ssize_t ret;

	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);
	ret = kstrtou8(buf, 0, &val);
	if (ret) {
		pr_err("%s: invalid input to aftask\n", __func__);
		return ret;
	}
	if (aw87xxx->denoise_flag) {
		mutex_lock(&aw87xxx->lock);
		if (!val)
			aw87xxx_kthread_toggle(aw87xxx, false);
		else
			aw87xxx_kthread_toggle(aw87xxx, true);
		mutex_unlock(&aw87xxx->lock);
	}

	return len;
}

static ssize_t aw87xxx_get_aftask(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t len = 0;

	mutex_lock(&aw87xxx->lock);
	len += snprintf(buf + len, PAGE_SIZE - len, "%d\n",
			(aw87xxx->actflag_sync_task != NULL));
	mutex_unlock(&aw87xxx->lock);
	return len;
}

static ssize_t aw87xxx_set_auto_thread(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	uint8_t val;
	ssize_t ret;

	pr_info("%s enter\n", __func__);
	ret = kstrtou8(buf, 0, &val);
	if (ret) {
		pr_err("%s: invalid input to auto_thread\n", __func__);
		return ret;
	}
	mutex_lock(&aw87xxx->lock);
	if (!val) {
		aw87xxx->actflag_auto_run = false;
	} else {
		aw87xxx->actflag_auto_run = true;
		if (aw87xxx->current_mode != AW87XXX_OFF_MODE &&
			aw87xxx->denoise_flag)
			aw87xxx_kthread_toggle(aw87xxx, true);
	}
	mutex_unlock(&aw87xxx->lock);
	return len;
}

static ssize_t aw87xxx_get_auto_thread(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t len = 0;

	mutex_lock(&aw87xxx->lock);
	len += snprintf(buf + len, PAGE_SIZE - len, "%d\n",
			aw87xxx->actflag_auto_run);
	mutex_unlock(&aw87xxx->lock);
	return len;
}

static ssize_t aw87xxx_set_actgain(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	unsigned int databuf[5] = { 0 };

	pr_info("%s enter\n", __func__);
	if (sscanf
	    (buf, "%x %x %x %x %x", &databuf[0], &databuf[1], &databuf[2],
	     &databuf[3], &databuf[4]) == 5) {
		mutex_lock(&aw87xxx->lock);
		memcpy(aw87xxx->actflag_gain_array, databuf, sizeof(databuf));
		mutex_unlock(&aw87xxx->lock);
	}
	return len;
}

static ssize_t aw87xxx_get_actgain(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	mutex_lock(&aw87xxx->lock);
	for (i = 0; i < 5; i++)
		len += snprintf(buf + len, PAGE_SIZE - len,
				"actflag_gain_array[%d] = 0x%02X\n", i,
				aw87xxx->actflag_gain_array[i]);
	mutex_unlock(&aw87xxx->lock);
	return len;
}

static ssize_t aw87xxx_get_registers(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
  struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
  ssize_t len = 0;
  unsigned int i = 0;
  unsigned char reg_val = 0;

  aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n",
		  __func__, aw87xxx_support_product[aw87xxx->product],
		  aw87xxx->i2c_seq, aw87xxx->i2c_addr);
  for (i = 0; i < aw87xxx->reg_param.reg_max; i++) {
	  if (!(aw87xxx->reg_param.reg_access[i] & AW87XXX_REG_RD_ACCESS))
		  continue;
	  aw87xxx_i2c_read(aw87xxx, i, &reg_val);
	  len += snprintf(buf + len, PAGE_SIZE - len,
			  "0x%02X: 0x%02X\n", i, reg_val);
  }
  return len;
}

static ssize_t aw87xxx_set_registers(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
 struct aw87xxx *aw87xxx = dev_get_drvdata(dev);
 unsigned int databuf[2] = { 0 };

 if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) == 2)
	 aw87xxx_i2c_write(aw87xxx, databuf[0], databuf[1]);
 return len;
}

static DEVICE_ATTR(reg, S_IWUSR | S_IRUGO, aw87xxx_get_reg, aw87xxx_set_reg);
static DEVICE_ATTR(hwen, S_IWUSR | S_IRUGO, aw87xxx_get_hwen, aw87xxx_set_hwen);
static DEVICE_ATTR(update, S_IWUSR, NULL, aw87xxx_set_update);
static DEVICE_ATTR(mode, S_IWUSR | S_IRUGO, aw87xxx_get_mode, aw87xxx_set_mode);
static DEVICE_ATTR(actflag, S_IRUGO, aw87xxx_get_actflag, NULL);
static DEVICE_ATTR(aftask, S_IWUSR | S_IRUGO, aw87xxx_get_aftask,
		   aw87xxx_set_aftask);
static DEVICE_ATTR(auto_thread, S_IWUSR | S_IRUGO, aw87xxx_get_auto_thread,
		   aw87xxx_set_auto_thread);
static DEVICE_ATTR(actgain, S_IWUSR | S_IRUGO, aw87xxx_get_actgain,
		   aw87xxx_set_actgain);
static DEVICE_ATTR(registers, S_IWUSR | S_IRUGO, aw87xxx_get_registers, aw87xxx_set_registers);

static struct attribute *aw87xxx_attributes[] = {
	&dev_attr_reg.attr,
	&dev_attr_hwen.attr,
	&dev_attr_update.attr,
	&dev_attr_mode.attr,
	&dev_attr_actflag.attr,
	&dev_attr_aftask.attr,
	&dev_attr_auto_thread.attr,
	&dev_attr_actgain.attr,
	&dev_attr_registers.attr,
	NULL
};

static struct attribute_group aw87xxx_attribute_group = {
	.attrs = aw87xxx_attributes
};

/****************************************************************************
* aw87xxx parse dts
*****************************************************************************/
static void aw87xxx_parse_gpio_dt(struct aw87xxx *aw87xxx,
				  struct device_node *np)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	aw87xxx->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
	if (aw87xxx->reset_gpio < 0) {
		aw_dev_err(aw87xxx->dev,
			   "%s: no reset gpio provided, hardware reset unavailable\n",
			   __func__);
		aw87xxx->reset_gpio = -1;
	} else {
		aw_dev_info(aw87xxx->dev, "%s: reset gpio provided ok\n",
			    __func__);
	}
}

static void aw87xxx_int_to_str(struct aw87xxx *aw87xxx)
{
	int32_t temp_channel = aw87xxx->pa_channel;

	aw_dev_info(aw87xxx->dev,
		    "%s enter, dev_i2c%d@0x%02X, pa_channel: %d\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr, aw87xxx->pa_channel);
	if (temp_channel >= 0) {
		snprintf(aw87xxx->channel, sizeof(aw87xxx->channel) + 1, "%d",
			 temp_channel);
		aw_dev_info(aw87xxx->dev, "%s: pa_channel: %s", __func__,
			    aw87xxx->channel);
	} else {
		aw87xxx->channel[0] = '-';
		aw87xxx->pa_channel = 0;
	}
}

static void aw87xxx_parse_channel_dt(struct aw87xxx *aw87xxx,
				     struct device_node *np)
{
	int ret = 0;

	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	snprintf(aw87xxx->bus_num, sizeof(aw87xxx->bus_num) + 1, "%d",
		 aw87xxx->i2c_seq);
	if (aw87xxx->i2c_addr == 0x58) {
		aw87xxx->name_suffix = "58";
	} else if (aw87xxx->i2c_addr == 0x59) {
		aw87xxx->name_suffix = "59";
	} else if (aw87xxx->i2c_addr == 0x5a) {
		aw87xxx->name_suffix = "5a";
	} else if (aw87xxx->i2c_addr == 0x5b) {
		aw87xxx->name_suffix = "5b";
	} else {
		aw_dev_info(aw87xxx->dev,
			    "%s: not stereo channel, use default single track!\n",
			    __func__);
		goto err_use_default;
	}
	ret = of_property_read_u32(np, "pa-channel", &aw87xxx->pa_channel);
	if (ret != 0) {
		aw87xxx->pa_channel = 0;
		aw_dev_err(aw87xxx->dev,
			   "%s: input error, user default value! ret:%d, pa_channel: %d\n",
			   __func__, ret, aw87xxx->pa_channel);
	} else {
		aw_dev_info(aw87xxx->dev, "%s: pa_channel: %d\n", __func__,
			    aw87xxx->pa_channel);
	}
	aw87xxx_int_to_str(aw87xxx);

	return;

 err_use_default:
	aw87xxx->name_suffix = NULL;
}

static int aw87xxx_parse_actflag_dt(struct aw87xxx *aw87xxx,
				    struct device_node *np)
{
	int ret, i;
	unsigned int array_tmp[5] = { 0x00, 0x08, 0x08, 0x08, 0x00 };

	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	ret = of_property_read_u32_array(np, "actflag-gain-array", array_tmp,
					 ARRAY_SIZE(array_tmp));
	if (ret != 0) {
		aw_dev_err(aw87xxx->dev,
			   "%s: actflag-gain-array not found, use default! ret = %d\n",
			   __func__, ret);
		memcpy(aw87xxx->actflag_gain_array, array_tmp,
			sizeof(array_tmp));
	} else {
		memcpy(aw87xxx->actflag_gain_array, array_tmp,
		       sizeof(array_tmp));
		for (i = 0; i < 5; i++)
			aw_dev_info(aw87xxx->dev,
				    "%s: aw87xxx->actflag_gain_array[%d] = 0x%02X\n",
				    __func__, i,
				    aw87xxx->actflag_gain_array[i]);
	}
	aw87xxx->actflag_auto_run =
	    of_property_read_bool(np, "actflag-auto-run");
	aw_dev_info(aw87xxx->dev, "%s: aw87xxx->actflag_auto_run = %d\n",
		    __func__, aw87xxx->actflag_auto_run);

	ret = of_property_read_u32(np, "gain-reg-addr", &aw87xxx->gain_reg_addr);
	if (ret != 0) {
		aw87xxx->denoise_flag = false;
		aw_dev_err(aw87xxx->dev,
			"%s: gain-reg-addr failed ,denoise will disable!\n",
			__func__);
	} else {
		aw87xxx->denoise_flag = true;
		aw_dev_info(aw87xxx->dev, "%s, gain_reg_addr: = 0x%02u\n",
			__func__, aw87xxx->gain_reg_addr);
	}
	return 0;
}

static void aw87xxx_parse_dt(struct aw87xxx *aw87xxx, struct device_node *np)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	aw87xxx_parse_gpio_dt(aw87xxx, np);
	aw87xxx_parse_monitor_dt(&aw87xxx->monitor);
	aw87xxx_parse_channel_dt(aw87xxx, np);
	aw87xxx_parse_actflag_dt(aw87xxx, np);
}

/*****************************************************
* check chip id
*****************************************************/
static int aw87xxx_update_cfg_name(struct aw87xxx *aw87xxx)
{
	char aw87xxx_head[] = { "aw87xxx_pid_" };
	char aw87xxx_end[] = { ".bin" };
	char buf[3] = { 0 };
	int i = 0;
	uint8_t head_index = 0;
	int cfg_size = 0;

	memcpy(aw87xxx->cfg_name, aw87xxx_cfg_name, sizeof(aw87xxx_cfg_name));
	head_index = sizeof(aw87xxx_head) - 1;

	/*add product information */
	snprintf(buf, sizeof(buf) + 1, "%02x", aw87xxx->chipid);

	for (i = 0; i < sizeof(aw87xxx->cfg_name) / AW87XXX_CFG_NAME_MAX; i++)
		memcpy(aw87xxx->cfg_name[i] + head_index, buf, sizeof(buf) - 1);

	for (i = 0; i < sizeof(aw87xxx->cfg_name) / AW87XXX_CFG_NAME_MAX; i++) {
		if (aw87xxx->channel[0] != '-') {
			cfg_size = strlen(aw87xxx->cfg_name[i]);
			memcpy(aw87xxx->cfg_name[i] + cfg_size,
			       aw87xxx->channel, strlen(aw87xxx->channel));
			cfg_size = strlen(aw87xxx->cfg_name[i]);
			memcpy(aw87xxx->cfg_name[i] + cfg_size, aw87xxx_end,
			       strlen(aw87xxx_end));
			aw_dev_info(aw87xxx->dev, "%s: cfg_name: %s\n",
				    __func__, aw87xxx->cfg_name[i]);
		} else {
			cfg_size = strlen(aw87xxx->cfg_name[i]);
			memcpy(aw87xxx->cfg_name[i] + cfg_size - 1, aw87xxx_end,
			       strlen(aw87xxx_end));
			aw_dev_info(aw87xxx->dev, "%s: cfg_name: %s\n",
				    __func__, aw87xxx->cfg_name[i]);
		}
	}

	/*vamx bin */
	if (aw87xxx->channel[0] != '-') {
		memcpy(aw87xxx->vmax_cfg_name, aw87xxx_vmax_cfg_name,
		       strlen(aw87xxx_vmax_cfg_name));
		memcpy(aw87xxx->vmax_cfg_name + strlen(aw87xxx->vmax_cfg_name),
		       aw87xxx->channel, strlen(aw87xxx->channel));
		memcpy(aw87xxx->vmax_cfg_name + strlen(aw87xxx->vmax_cfg_name),
		       aw87xxx_end, strlen(aw87xxx_end));
		aw_dev_info(aw87xxx->dev,
			    "%s: dev_i2c%d@0x%02X, vmax_name: %s\n", __func__,
			    aw87xxx->i2c_seq, aw87xxx->i2c_addr,
			    aw87xxx->vmax_cfg_name);
	} else {
		memcpy(aw87xxx->vmax_cfg_name, aw87xxx_vmax_cfg_name,
		       strlen(aw87xxx_vmax_cfg_name));
		memcpy(aw87xxx->vmax_cfg_name + strlen(aw87xxx->vmax_cfg_name) - 1,
		       aw87xxx_end, strlen(aw87xxx_end));
		aw_dev_info(aw87xxx->dev,
			    "%s: dev_i2c%d@0x%02X, vmax_name: %s\n", __func__,
			    aw87xxx->i2c_seq, aw87xxx->i2c_addr,
			    aw87xxx->vmax_cfg_name);
	}

	return 0;
}

static int aw87xxx_read_chipid(struct aw87xxx *aw87xxx)
{
	unsigned int cnt = 0;
	int ret = -1;
	unsigned char reg_val = 0;

	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
			aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	while (cnt < AW_READ_CHIPID_RETRIES) {
		ret = aw87xxx_i2c_read(aw87xxx, AW87XXX_REG_CHIPID, &reg_val);
		aw_dev_info(aw87xxx->dev, "%s: 0x%x\n", __func__, reg_val);
		switch (reg_val) {
		case AW87XXX_CHIPID_39:
			aw87xxx->chipid = reg_val;
			aw87xxx_product_select(aw87xxx, AW87XXX_339);
			aw87xxx_update_cfg_name(aw87xxx);
			return 0;
		case AW87XXX_CHIPID_59:
			aw87xxx->chipid = reg_val;
			if (gpio_is_valid(aw87xxx->reset_gpio))
				aw87xxx_product_select(aw87xxx, AW87XXX_519);
			else
				aw87xxx_product_select(aw87xxx, AW87XXX_359);
			aw87xxx_update_cfg_name(aw87xxx);
			return 0;
		case AW87XXX_CHIPID_69:
			aw87xxx->chipid = reg_val;
			aw87xxx_product_select(aw87xxx, AW87XXX_369);
			aw87xxx_update_cfg_name(aw87xxx);
			return 0;
		case AW87XXX_CHIPID_5A:
			aw87xxx->chipid = reg_val;
			aw87xxx_product_select(aw87xxx, AW87XXX_559);
			aw87xxx_update_cfg_name(aw87xxx);
			aw_dev_info(aw87xxx->dev,
				    "%s: find device chip id is (0x%x)\n",
				    __func__, reg_val);
			return 0;
		default:
			aw_dev_info(aw87xxx->dev,
				    "%s: unsupported device revision (0x%x)\n",
				    __func__, reg_val);
			break;
		}
		cnt++;
		mdelay(AW_READ_CHIPID_RETRY_DELAY);
	}
	aw_dev_err(aw87xxx->dev, "%s: aw87xxx chipid=0x%x error\n",
		   __func__, reg_val);
	return -EINVAL;
}

/****************************************************************************
* aw87xxx i2c driver
*****************************************************************************/
static int aw87xxx_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct device_node *np = client->dev.of_node;
	struct aw87xxx *aw87xxx = NULL;
	int ret = -1;

	pr_info("%s enter, dev_i2c%d@0x%02X\n", __func__,
		client->adapter->nr, client->addr);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		aw_dev_err(&client->dev, "%s: check_functionality failed\n",
			   __func__);
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}
	aw87xxx =
	    devm_kzalloc(&client->dev, sizeof(struct aw87xxx), GFP_KERNEL);
	if (aw87xxx == NULL) {
		dev_err(&client->dev, "%s: devm_kzalloc failed.\n", __func__);
		ret = -ENOMEM;
		goto err_devm_kzalloc_failed;
	}
	aw87xxx->i2c_client = client;
	aw87xxx->dev = &client->dev;
	i2c_set_clientdata(client, aw87xxx);
	aw87xxx->i2c_seq = aw87xxx->i2c_client->adapter->nr;
	aw87xxx->i2c_addr = aw87xxx->i2c_client->addr;
	INIT_LIST_HEAD(&aw87xxx->list);
	mutex_init(&aw87xxx->lock);
	aw87xxx_parse_dt(aw87xxx, np);
	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		ret = devm_gpio_request_one(&client->dev,
					    aw87xxx->reset_gpio,
					    GPIOF_OUT_INIT_LOW, "aw87xxx_rst");
		if (ret) {
			aw_dev_err(&client->dev,
				   "%s: rst request failed\n", __func__);
			goto err_gpio_request_failed;
		}
	}
	aw87xxx_hw_reset(aw87xxx);
	/* aw87xxx chip id */
	ret = aw87xxx_read_chipid(aw87xxx);
	if (ret < 0) {
		aw_dev_err(&client->dev,
			   "%s: aw873xx_read_chipid failed ret=%d\n", __func__,
			   ret);
		goto err_check_id_failed;
	}
	if (aw87xxx->product == AW87XXX_339 ||
	    aw87xxx->product == AW87XXX_359 ||
	    aw87xxx->product == AW87XXX_369) {
		aw_dev_info(aw87xxx->dev, "%s: aw873x9 always hw on!!!\n",
			    __func__);
		aw87xxx->hwen_flag = 1;
	}
	/* aw87xxx device name */
	if (client->dev.of_node) {
		if (aw87xxx->name_suffix)
			dev_set_name(&client->dev, "%s_%s_%s", "aw87xxx_pa",
				     aw87xxx->bus_num, aw87xxx->name_suffix);
		else
			dev_set_name(&client->dev, "%s", "aw87xxx_pa");
	} else {
		aw_dev_err(&client->dev, "%s: failed to set device name: %d\n",
			   __func__, ret);
	}
	ret = sysfs_create_group(&client->dev.kobj, &aw87xxx_attribute_group);
	if (ret < 0) {
		aw_dev_err(&client->dev,
			   "%s: failed to create sysfs nodes\n", __func__);
	}
	/*add device to total list */
	mutex_lock(&g_aw87xxx_mutex_lock);
	g_aw87xxx_dev_cnt++;
	list_add(&aw87xxx->list, &g_aw87xxx_device_list);
	mutex_unlock(&g_aw87xxx_mutex_lock);
	spin_lock_init(&aw87xxx->bin_parse_lock);
#ifdef CONFIG_SND_SOC_AW87XXX
	g_aw87xxx = aw87xxx;
#endif
#ifndef AW87XXX_EVT_BUILD
	aw87xxx_cfg_init(aw87xxx);
#endif
	aw87xxx_hw_off(aw87xxx);
	aw87xxx_monitor_init(&aw87xxx->monitor);
	return 0;

 err_check_id_failed:
	if (gpio_is_valid(aw87xxx->reset_gpio))
		devm_gpio_free(&client->dev, aw87xxx->reset_gpio);
 err_gpio_request_failed:
	devm_kfree(&client->dev, aw87xxx);
	aw87xxx = NULL;
#ifdef CONFIG_SND_SOC_AW87XXX
	g_aw87xxx = NULL;
#endif
 err_devm_kzalloc_failed:
 err_check_functionality_failed:
	return ret;
}

#ifdef CONFIG_SND_SOC_AW87XXX
int soc_check_aw87xxx_status()
{
	int ret;
	struct aw87xxx *aw87xxx = g_aw87xxx;

	if (aw87xxx == NULL) {
		pr_err("%s  aw87xxx not ready", __func__);
		return 0;
	}
	aw87xxx_hw_reset(aw87xxx);
	ret = aw87xxx_read_chipid(aw87xxx);
	if (ret < 0) {
		pr_err("%s: aw873xx_read_chipid failed ret=%d\n", __func__,ret);
		return 0;
	}
	aw87xxx_hw_off(aw87xxx);

	pr_info("%s aw87xxx is %s available", __func__, ret==0?"":"not");
	return (ret == 0)?1:0;
}
EXPORT_SYMBOL(soc_check_aw87xxx_status);
#endif

static int aw87xxx_i2c_remove(struct i2c_client *client)
{
	struct aw87xxx *aw87xxx = i2c_get_clientdata(client);

	if (gpio_is_valid(aw87xxx->reset_gpio))
		devm_gpio_free(&client->dev, aw87xxx->reset_gpio);
	list_del(&aw87xxx->list);
	devm_kfree(&client->dev, aw87xxx);
	aw87xxx = NULL;
#ifdef CONFIG_SND_SOC_AW87XXX
	g_aw87xxx = NULL;
#endif
	return 0;
}

static void aw87xxx_i2c_shutdown(struct i2c_client *client)
{
	struct aw87xxx *aw87xxx = i2c_get_clientdata(client);

	pr_info("%s enter!\n", __func__);
	aw87xxx_audio_off(aw87xxx);
}

static const struct i2c_device_id aw87xxx_i2c_id[] = {
	{AW87XXX_I2C_NAME, 0},
	{}
};

static const struct of_device_id extpa_of_match[] = {
	{.compatible = "awinic,aw87xxx_pa_58"},
	{.compatible = "awinic,aw87xxx_pa_59"},
	/*
	 * {.compatible = "awinic,aw87xxx_pa_5A"},
	 * {.compatible = "awinic,aw87xxx_pa_5B"},
	 */
	{}
};

static struct i2c_driver aw87xxx_i2c_driver = {
	.driver = {
		   .owner = THIS_MODULE,
		   .name = AW87XXX_I2C_NAME,
		   .of_match_table = extpa_of_match,
		   },
	.probe = aw87xxx_i2c_probe,
	.remove = aw87xxx_i2c_remove,
	.shutdown = aw87xxx_i2c_shutdown,
	.id_table = aw87xxx_i2c_id,
};

static int __init aw87xxx_pa_init(void)
{
	int ret;

	pr_info("%s enter, driver version: %s\n", __func__,
		AW87XXX_DRIVER_VERSION);
	ret = i2c_add_driver(&aw87xxx_i2c_driver);
	if (ret) {
		pr_info("%s: unable to register driver: %d\n", __func__, ret);
		return ret;
	}
	return 0;
}

module_init(aw87xxx_pa_init);

static void __exit aw87xxx_pa_exit(void)
{
	pr_info("%s enter\n", __func__);
	i2c_del_driver(&aw87xxx_i2c_driver);
}

module_exit(aw87xxx_pa_exit);

MODULE_AUTHOR("<zhaolei@awinic.com>");
MODULE_DESCRIPTION("awinic aw87xxx pa driver");
MODULE_LICENSE("GPL v2");
