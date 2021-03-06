/*
 * Synaptics TCM touchscreen driver
 *
 * Copyright (C) 2017-2018 Synaptics Incorporated. All rights reserved.
 *
 * Copyright (C) 2017-2018 Scott Lin <scott.lin@tw.synaptics.com>
 * Copyright (C) 2018-2019 Ian Su <ian.su@tw.synaptics.com>
 * Copyright (C) 2018-2019 Joey Zhou <joey.zhou@synaptics.com>
 * Copyright (C) 2018-2019 Yuehao Qiu <yuehao.qiu@synaptics.com>
 * Copyright (C) 2018-2019 Aaron Chen <aaron.chen@tw.synaptics.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * INFORMATION CONTAINED IN THIS DOCUMENT IS PROVIDED "AS-IS," AND SYNAPTICS
 * EXPRESSLY DISCLAIMS ALL EXPRESS AND IMPLIED WARRANTIES, INCLUDING ANY
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE,
 * AND ANY WARRANTIES OF NON-INFRINGEMENT OF ANY INTELLECTUAL PROPERTY RIGHTS.
 * IN NO EVENT SHALL SYNAPTICS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, PUNITIVE, OR CONSEQUENTIAL DAMAGES ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OF THE INFORMATION CONTAINED IN THIS DOCUMENT, HOWEVER CAUSED
 * AND BASED ON ANY THEORY OF LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, AND EVEN IF SYNAPTICS WAS ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE. IF A TRIBUNAL OF COMPETENT JURISDICTION DOES
 * NOT PERMIT THE DISCLAIMER OF DIRECT DAMAGES OR ANY OTHER DAMAGES, SYNAPTICS'
 * TOTAL CUMULATIVE LIABILITY TO ANY PARTY SHALL NOT EXCEED ONE HUNDRED U.S.
 * DOLLARS.
 */

#include <linux/gpio.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include "synaptics_tcm_core.h"

/* #define RESET_ON_RESUME */

/* #define RESUME_EARLY_UNBLANK */

#define RESET_ON_RESUME_DELAY_MS 50

#define PREDICTIVE_READING

#define MIN_READ_LENGTH 9

/* #define FORCE_RUN_APPLICATION_FIRMWARE */

#define NOTIFIER_PRIORITY 2

#define RESPONSE_TIMEOUT_MS 3000

#define APP_STATUS_POLL_TIMEOUT_MS 1000

#define APP_STATUS_POLL_MS 100

#define ENABLE_IRQ_DELAY_MS 20

#define FALL_BACK_ON_POLLING

#define POLLING_DELAY_MS 5

#define MODE_SWITCH_DELAY_MS 100

#define READ_RETRY_US_MIN 5000

#define READ_RETRY_US_MAX 10000

#define WRITE_DELAY_US_MIN 500

#define WRITE_DELAY_US_MAX 1000

#define ROMBOOT_DOWNLOAD_UNIT 16

#define PDT_END_ADDR 0x00ee

#define RMI_UBL_FN_NUMBER 0x35

static struct syna_tcm_module_pool mod_pool;
bool shutdown_is_on_going_tsp;

DECLARE_COMPLETION(response_complete);

static int syna_tcm_sensor_detection(struct syna_tcm_hcd *tcm_hcd);
static void syna_tcm_check_hdl(struct syna_tcm_hcd *tcm_hcd,
							unsigned char id);
int syna_tcm_add_module(struct syna_tcm_module_cb *mod_cb, bool insert)
{
	struct syna_tcm_module_handler *mod_handler;

	if (!mod_pool.initialized) {
		mutex_init(&mod_pool.mutex);
		INIT_LIST_HEAD(&mod_pool.list);
		mod_pool.initialized = true;
	}

	mutex_lock(&mod_pool.mutex);

	if (insert) {
		mod_handler = kzalloc(sizeof(*mod_handler), GFP_KERNEL);
		if (!mod_handler) {
			pr_err("%s: Failed to allocate memory for mod_handler\n", __func__);
			mutex_unlock(&mod_pool.mutex);
			return -ENOMEM;
		}
		mod_handler->mod_cb = mod_cb;
		mod_handler->insert = true;
		mod_handler->detach = false;
		list_add_tail(&mod_handler->link, &mod_pool.list);
	} else if (!list_empty(&mod_pool.list)) {
		list_for_each_entry(mod_handler, &mod_pool.list, link) {
			if (mod_handler->mod_cb->type == mod_cb->type) {
				mod_handler->insert = false;
				mod_handler->detach = true;
				goto exit;
			}
		}
	}

exit:
	mutex_unlock(&mod_pool.mutex);

	if (mod_pool.queue_work)
		queue_work(mod_pool.workqueue, &mod_pool.work);

	return 0;
}
EXPORT_SYMBOL(syna_tcm_add_module);

static void syna_tcm_module_work(struct work_struct *work)
{
	struct syna_tcm_module_handler *mod_handler;
	struct syna_tcm_module_handler *tmp_handler;
	struct syna_tcm_hcd *tcm_hcd = mod_pool.tcm_hcd;

	mutex_lock(&mod_pool.mutex);

	if (!list_empty(&mod_pool.list)) {
		list_for_each_entry_safe(mod_handler, tmp_handler, &mod_pool.list, link) {
			if (mod_handler->insert) {
				if (mod_handler->mod_cb->init)
					mod_handler->mod_cb->init(tcm_hcd);
				mod_handler->insert = false;
			}
			if (mod_handler->detach) {
				if (mod_handler->mod_cb->remove)
					mod_handler->mod_cb->remove(tcm_hcd);
				list_del(&mod_handler->link);
				kfree(mod_handler);
			}
		}
	}

	mutex_unlock(&mod_pool.mutex);

	return;
}

void sec_ts_print_info(struct syna_tcm_hcd *tcm_hcd)
{
	if (!tcm_hcd)
		return;

	tcm_hcd->print_info_cnt_open++;

	if (tcm_hcd->print_info_cnt_open > 0xfff0)
		tcm_hcd->print_info_cnt_open = 0;

	if (tcm_hcd->touch_count == 0)
		tcm_hcd->print_info_cnt_release++;

	input_info(true, tcm_hcd->pdev->dev.parent,
		"tc:%d noise:%d Sensitivity:%d // v:%02X%02X // irq:%d //#%d %d\n", tcm_hcd->touch_count, tcm_hcd->noise,
		tcm_hcd->sensitivity_mode, tcm_hcd->app_info.customer_config_id[2], tcm_hcd->app_info.customer_config_id[3],
		gpio_get_value(tcm_hcd->hw_if->bdata->irq_gpio), tcm_hcd->print_info_cnt_open, tcm_hcd->print_info_cnt_release);
}

static void touch_print_info_work(struct work_struct *work)
{
	struct syna_tcm_hcd *tcm_hcd = container_of(work, struct syna_tcm_hcd,
			work_print_info.work);

	sec_ts_print_info(tcm_hcd);

	if (!shutdown_is_on_going_tsp)
		schedule_delayed_work(&tcm_hcd->work_print_info, msecs_to_jiffies(TOUCH_PRINT_INFO_DWORK_TIME));
}

static void sec_read_info_work(struct work_struct *work)
{
	struct syna_tcm_hcd *tcm_hcd = container_of(work, struct syna_tcm_hcd,
			work_read_info.work);

	input_log_fix();
	sec_run_rawdata(tcm_hcd);
}

#ifdef REPORT_NOTIFIER
/**
 * syna_tcm_report_notifier() - notify occurrence of report received from device
 *
 * @data: handle of core module
 *
 * The occurrence of the report generated by the device is forwarded to the
 * asynchronous inbox of each registered application module.
 */
static int syna_tcm_report_notifier(void *data)
{
	struct sched_param param = { .sched_priority = NOTIFIER_PRIORITY };
	struct syna_tcm_module_handler *mod_handler;
	struct syna_tcm_hcd *tcm_hcd = data;

	sched_setscheduler(current, SCHED_RR, &param);

	set_current_state(TASK_INTERRUPTIBLE);

	while (!kthread_should_stop()) {
		schedule();

		if (kthread_should_stop())
			break;

		set_current_state(TASK_RUNNING);

		mutex_lock(&mod_pool.mutex);

		if (!list_empty(&mod_pool.list)) {
			list_for_each_entry(mod_handler, &mod_pool.list, link) {
				if (!mod_handler->insert && !mod_handler->detach &&
					(mod_handler->mod_cb->asyncbox))
					mod_handler->mod_cb->asyncbox(tcm_hcd);
			}
		}

		mutex_unlock(&mod_pool.mutex);

		set_current_state(TASK_INTERRUPTIBLE);
	};

	return 0;
}
#endif

/**
 * syna_tcm_dispatch_report() - dispatch report received from device
 *
 * @tcm_hcd: handle of core module
 *
 * The report generated by the device is forwarded to the synchronous inbox of
 * each registered application module for further processing. In addition, the
 * report notifier thread is woken up for asynchronous notification of the
 * report occurrence.
 */
static void syna_tcm_dispatch_report(struct syna_tcm_hcd *tcm_hcd)
{
	struct syna_tcm_module_handler *mod_handler;

	LOCK_BUFFER(tcm_hcd->in);
	LOCK_BUFFER(tcm_hcd->report.buffer);

	tcm_hcd->report.buffer.buf = &tcm_hcd->in.buf[MESSAGE_HEADER_SIZE];

	tcm_hcd->report.buffer.buf_size = tcm_hcd->in.buf_size;
	tcm_hcd->report.buffer.buf_size -= MESSAGE_HEADER_SIZE;

	tcm_hcd->report.buffer.data_length = tcm_hcd->payload_length;

	tcm_hcd->report.id = tcm_hcd->status_report_code;

	/* report directly if touch report is received */
	if (tcm_hcd->report.id == REPORT_TOUCH) {
		if (tcm_hcd->report_touch)
			tcm_hcd->report_touch();

	} else {

		/* once an identify report is received, */
		/* reinitialize touch in case any changes */
		if ((tcm_hcd->report.id == REPORT_IDENTIFY) &&
				IS_FW_MODE(tcm_hcd->id_info.mode)) {

			if (atomic_read(&tcm_hcd->helper.task) == HELP_NONE) {
				atomic_set(&tcm_hcd->helper.task, HELP_TOUCH_REINIT);
				queue_work(tcm_hcd->helper.workqueue, &tcm_hcd->helper.work);
			}
		}

		/* dispatch received report to the other modules */
		mutex_lock(&mod_pool.mutex);

		if (!list_empty(&mod_pool.list)) {
			list_for_each_entry(mod_handler, &mod_pool.list, link) {
				if (!mod_handler->insert && !mod_handler->detach &&
						(mod_handler->mod_cb->syncbox))
					mod_handler->mod_cb->syncbox(tcm_hcd);
			}
		}

		tcm_hcd->async_report_id = tcm_hcd->status_report_code;

		mutex_unlock(&mod_pool.mutex);
	}

	UNLOCK_BUFFER(tcm_hcd->report.buffer);
	UNLOCK_BUFFER(tcm_hcd->in);

#ifdef REPORT_NOTIFIER
	wake_up_process(tcm_hcd->notifier_thread);
#endif

	return;
}

/**
 * syna_tcm_dispatch_response() - dispatch response received from device
 *
 * @tcm_hcd: handle of core module
 *
 * The response to a command is forwarded to the sender of the command.
 */
static void syna_tcm_dispatch_response(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;

	if (atomic_read(&tcm_hcd->command_status) != CMD_BUSY)
		return;

	tcm_hcd->response_code = tcm_hcd->status_report_code;

	if (tcm_hcd->payload_length == 0) {
		atomic_set(&tcm_hcd->command_status, CMD_IDLE);
		goto exit;
	}

	LOCK_BUFFER(tcm_hcd->resp);

	retval = syna_tcm_alloc_mem(tcm_hcd, &tcm_hcd->resp, tcm_hcd->payload_length);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
						"Failed to allocate memory for tcm_hcd->resp.buf\n");
		UNLOCK_BUFFER(tcm_hcd->resp);
		atomic_set(&tcm_hcd->command_status, CMD_ERROR);
		goto exit;
	}

	LOCK_BUFFER(tcm_hcd->in);

	retval = secure_memcpy(tcm_hcd->resp.buf,
				tcm_hcd->resp.buf_size, &tcm_hcd->in.buf[MESSAGE_HEADER_SIZE],
				tcm_hcd->in.buf_size - MESSAGE_HEADER_SIZE,
				tcm_hcd->payload_length);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy payload\n");
		UNLOCK_BUFFER(tcm_hcd->in);
		UNLOCK_BUFFER(tcm_hcd->resp);
		atomic_set(&tcm_hcd->command_status, CMD_ERROR);
		goto exit;
	}

	tcm_hcd->resp.data_length = tcm_hcd->payload_length;

	UNLOCK_BUFFER(tcm_hcd->in);
	UNLOCK_BUFFER(tcm_hcd->resp);

	atomic_set(&tcm_hcd->command_status, CMD_IDLE);

exit:
	complete(&response_complete);

	return;
}

/**
 * syna_tcm_dispatch_message() - dispatch message received from device
 *
 * @tcm_hcd: handle of core module
 *
 * The information received in the message read in from the device is dispatched
 * to the appropriate destination based on whether the information represents a
 * report or a response to a command.
 */
static void syna_tcm_dispatch_message(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *build_id;
	unsigned int payload_length;
	unsigned int max_write_size;

	if (tcm_hcd->status_report_code == REPORT_IDENTIFY) {
		payload_length = tcm_hcd->payload_length;

		LOCK_BUFFER(tcm_hcd->in);

		retval = secure_memcpy((unsigned char *)&tcm_hcd->id_info,
					sizeof(tcm_hcd->id_info), &tcm_hcd->in.buf[MESSAGE_HEADER_SIZE],
					tcm_hcd->in.buf_size - MESSAGE_HEADER_SIZE,
					MIN(sizeof(tcm_hcd->id_info), payload_length));
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to copy identification info\n");
			UNLOCK_BUFFER(tcm_hcd->in);
			return;
		}

		UNLOCK_BUFFER(tcm_hcd->in);

		build_id = tcm_hcd->id_info.build_id;
		tcm_hcd->packrat_number = le4_to_uint(build_id);

		max_write_size = le2_to_uint(tcm_hcd->id_info.max_write_size);
		tcm_hcd->wr_chunk_size = MIN(max_write_size, WR_CHUNK_SIZE);
		if (tcm_hcd->wr_chunk_size == 0)
			tcm_hcd->wr_chunk_size = max_write_size;

		input_info(true, tcm_hcd->pdev->dev.parent,
			"Received identify report (firmware mode = 0x%02x)\n", tcm_hcd->id_info.mode);

		if (atomic_read(&tcm_hcd->command_status) == CMD_BUSY) {
			switch (tcm_hcd->command) {
			case CMD_RESET:
			case CMD_RUN_BOOTLOADER_FIRMWARE:
			case CMD_RUN_APPLICATION_FIRMWARE:
			case CMD_ENTER_PRODUCTION_TEST_MODE:
			case CMD_ROMBOOT_RUN_BOOTLOADER_FIRMWARE:
				tcm_hcd->response_code = STATUS_OK;
				atomic_set(&tcm_hcd->command_status, CMD_IDLE);
				complete(&response_complete);
				break;
			default:
				input_info(true, tcm_hcd->pdev->dev.parent, "Device has been reset\n");
				atomic_set(&tcm_hcd->command_status, CMD_ERROR);
				complete(&response_complete);
				break;
			}
		} else {

			if ((tcm_hcd->id_info.mode == MODE_ROMBOOTLOADER) && tcm_hcd->in_hdl_mode) {

				if (atomic_read(&tcm_hcd->helper.task) == HELP_NONE) {
					atomic_set(&tcm_hcd->helper.task, HELP_SEND_ROMBOOT_HDL);
					queue_work(tcm_hcd->helper.workqueue, &tcm_hcd->helper.work);
				} else {
					input_info(true, tcm_hcd->pdev->dev.parent, "Helper thread is busy\n");
				}
				return;
			}
		}

#ifdef FORCE_RUN_APPLICATION_FIRMWARE
		if (IS_NOT_FW_MODE(tcm_hcd->id_info.mode) && !mutex_is_locked(&tcm_hcd->reset_mutex)) {
			if (atomic_read(&tcm_hcd->helper.task) == HELP_NONE) {
				atomic_set(&tcm_hcd->helper.task, HELP_RUN_APPLICATION_FIRMWARE);
				queue_work(tcm_hcd->helper.workqueue, &tcm_hcd->helper.work);
				return;
			}
		}
#endif

		/* To avoid the identify report dispatching during the HDL. */
		if (atomic_read(&tcm_hcd->host_downloading)) {
			input_info(true, tcm_hcd->pdev->dev.parent,
				"Switched to TCM mode and going to download the configs\n");
			return;
		}
	}


	if (tcm_hcd->status_report_code >= REPORT_IDENTIFY)
		syna_tcm_dispatch_report(tcm_hcd);
	else
		syna_tcm_dispatch_response(tcm_hcd);

	return;
}

/**
 * syna_tcm_continued_read() - retrieve entire payload from device
 *
 * @tcm_hcd: handle of core module
 *
 * Read transactions are carried out until the entire payload is retrieved from
 * the device and stored in the handle of the core module.
 */
static int syna_tcm_continued_read(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char marker;
	unsigned char code;
	unsigned int idx;
	unsigned int offset;
	unsigned int chunks;
	unsigned int chunk_space;
	unsigned int xfer_length;
	unsigned int total_length;
	unsigned int remaining_length;

	total_length = MESSAGE_HEADER_SIZE + tcm_hcd->payload_length + 1;

	remaining_length = total_length - tcm_hcd->read_length;

	LOCK_BUFFER(tcm_hcd->in);

	retval = syna_tcm_realloc_mem(tcm_hcd, &tcm_hcd->in, total_length + 1);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to reallocate memory for tcm_hcd->in.buf\n");
		UNLOCK_BUFFER(tcm_hcd->in);
		return retval;
	}

	/* available chunk space for payload = total chunk size minus header
	 * marker byte and header code byte */
	if (tcm_hcd->rd_chunk_size == 0)
		chunk_space = remaining_length;
	else
		chunk_space = tcm_hcd->rd_chunk_size - 2;

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	offset = tcm_hcd->read_length;

	LOCK_BUFFER(tcm_hcd->temp);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space)
			xfer_length = chunk_space;
		else
			xfer_length = remaining_length;

		if (xfer_length == 1) {
			tcm_hcd->in.buf[offset] = MESSAGE_PADDING;
			offset += xfer_length;
			remaining_length -= xfer_length;
			continue;
		}

		retval = syna_tcm_alloc_mem(tcm_hcd, &tcm_hcd->temp, xfer_length + 2);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to allocate memory for tcm_hcd->temp.buf\n");
			UNLOCK_BUFFER(tcm_hcd->temp);
			UNLOCK_BUFFER(tcm_hcd->in);
			return retval;
		}

		retval = syna_tcm_read(tcm_hcd,	tcm_hcd->temp.buf, xfer_length + 2);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to read from device\n");
			UNLOCK_BUFFER(tcm_hcd->temp);
			UNLOCK_BUFFER(tcm_hcd->in);
			return retval;
		}

		marker = tcm_hcd->temp.buf[0];
		code = tcm_hcd->temp.buf[1];

		if (marker != MESSAGE_MARKER) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Incorrect header marker (0x%02x)\n", marker);
			UNLOCK_BUFFER(tcm_hcd->temp);
			UNLOCK_BUFFER(tcm_hcd->in);
			return -EIO;
		}

		if (code != STATUS_CONTINUED_READ) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Incorrect header code (0x%02x)\n", code);
			UNLOCK_BUFFER(tcm_hcd->temp);
			UNLOCK_BUFFER(tcm_hcd->in);
			return -EIO;
		}

		retval = secure_memcpy(&tcm_hcd->in.buf[offset],
					tcm_hcd->in.buf_size - offset, &tcm_hcd->temp.buf[2],
					tcm_hcd->temp.buf_size - 2,	xfer_length);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy payload\n");
			UNLOCK_BUFFER(tcm_hcd->temp);
			UNLOCK_BUFFER(tcm_hcd->in);
			return retval;
		}

		offset += xfer_length;

		remaining_length -= xfer_length;
	}

	UNLOCK_BUFFER(tcm_hcd->temp);
	UNLOCK_BUFFER(tcm_hcd->in);

	return 0;
}

/**
 * syna_tcm_raw_read() - retrieve specific number of data bytes from device
 *
 * @tcm_hcd: handle of core module
 * @in_buf: buffer for storing data retrieved from device
 * @length: number of bytes to retrieve from device
 *
 * Read transactions are carried out until the specific number of data bytes are
 * retrieved from the device and stored in in_buf.
 */
static int syna_tcm_raw_read(struct syna_tcm_hcd *tcm_hcd,
		unsigned char *in_buf, unsigned int length)
{
	int retval;
	unsigned char code;
	unsigned int idx;
	unsigned int offset;
	unsigned int chunks;
	unsigned int chunk_space;
	unsigned int xfer_length;
	unsigned int remaining_length;

	if (length < 2) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Invalid length information\n");
		return -EINVAL;
	}

	/* minus header marker byte and header code byte */
	remaining_length = length - 2;

	/* available chunk space for data = total chunk size minus header marker
	 * byte and header code byte */
	if (tcm_hcd->rd_chunk_size == 0)
		chunk_space = remaining_length;
	else
		chunk_space = tcm_hcd->rd_chunk_size - 2;

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	offset = 0;

	LOCK_BUFFER(tcm_hcd->temp);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space)
			xfer_length = chunk_space;
		else
			xfer_length = remaining_length;

		if (xfer_length == 1) {
			in_buf[offset] = MESSAGE_PADDING;
			offset += xfer_length;
			remaining_length -= xfer_length;
			continue;
		}

		retval = syna_tcm_alloc_mem(tcm_hcd, &tcm_hcd->temp, xfer_length + 2);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to allocate memory for tcm_hcd->temp.buf\n");
			UNLOCK_BUFFER(tcm_hcd->temp);
			return retval;
		}

		retval = syna_tcm_read(tcm_hcd,	tcm_hcd->temp.buf, xfer_length + 2);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to read from device\n");
			UNLOCK_BUFFER(tcm_hcd->temp);
			return retval;
		}

		code = tcm_hcd->temp.buf[1];

		if (idx == 0) {
			retval = secure_memcpy(&in_buf[0], length,
						&tcm_hcd->temp.buf[0], tcm_hcd->temp.buf_size,
						xfer_length + 2);
		} else {
			if (code != STATUS_CONTINUED_READ) {
				input_err(true, tcm_hcd->pdev->dev.parent,
					"Incorrect header code (0x%02x)\n",	code);
				UNLOCK_BUFFER(tcm_hcd->temp);
				return -EIO;
			}

			retval = secure_memcpy(&in_buf[offset],
						length - offset, &tcm_hcd->temp.buf[2],
						tcm_hcd->temp.buf_size - 2, xfer_length);
		}
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy data\n");
			UNLOCK_BUFFER(tcm_hcd->temp);
			return retval;
		}

		if (idx == 0)
			offset += (xfer_length + 2);
		else
			offset += xfer_length;

		remaining_length -= xfer_length;
	}

	UNLOCK_BUFFER(tcm_hcd->temp);

	return 0;
}

/**
 * syna_tcm_raw_write() - write command/data to device without receiving
 * response
 *
 * @tcm_hcd: handle of core module
 * @command: command to send to device
 * @data: data to send to device
 * @length: length of data in bytes
 *
 * A command and its data, if any, are sent to the device.
 */
static int syna_tcm_raw_write(struct syna_tcm_hcd *tcm_hcd,
		unsigned char command, unsigned char *data, unsigned int length)
{
	int retval;
	unsigned int idx;
	unsigned int chunks;
	unsigned int chunk_space;
	unsigned int xfer_length;
	unsigned int remaining_length;

	remaining_length = length;

	/* available chunk space for data = total chunk size minus command
	 * byte */
	if (tcm_hcd->wr_chunk_size == 0)
		chunk_space = remaining_length;
	else
		chunk_space = tcm_hcd->wr_chunk_size - 1;

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	LOCK_BUFFER(tcm_hcd->out);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space)
			xfer_length = chunk_space;
		else
			xfer_length = remaining_length;

		retval = syna_tcm_alloc_mem(tcm_hcd, &tcm_hcd->out, xfer_length + 1);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to allocate memory for tcm_hcd->out.buf\n");
			UNLOCK_BUFFER(tcm_hcd->out);
			return retval;
		}

		if (idx == 0)
			tcm_hcd->out.buf[0] = command;
		else
			tcm_hcd->out.buf[0] = CMD_CONTINUE_WRITE;

		if (xfer_length) {
			retval = secure_memcpy(&tcm_hcd->out.buf[1],
						tcm_hcd->out.buf_size - 1, &data[idx * chunk_space],
						remaining_length, xfer_length);
			if (retval < 0) {
				input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy data\n");
				UNLOCK_BUFFER(tcm_hcd->out);
				return retval;
			}
		}

		retval = syna_tcm_write(tcm_hcd, tcm_hcd->out.buf, xfer_length + 1);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to write to device\n");
			UNLOCK_BUFFER(tcm_hcd->out);
			return retval;
		}

		remaining_length -= xfer_length;
	}

	UNLOCK_BUFFER(tcm_hcd->out);

	return 0;
}

/**
 * syna_tcm_read_message() - read message from device
 *
 * @tcm_hcd: handle of core module
 * @in_buf: buffer for storing data in raw read mode
 * @length: length of data in bytes in raw read mode
 *
 * If in_buf is not NULL, raw read mode is used and syna_tcm_raw_read() is
 * called. Otherwise, a message including its entire payload is retrieved from
 * the device and dispatched to the appropriate destination.
 */
static int syna_tcm_read_message(struct syna_tcm_hcd *tcm_hcd,
		unsigned char *in_buf, unsigned int length)
{
	int retval;
	bool retry;
	unsigned int total_length;
	struct syna_tcm_message_header *header;

	if (tcm_hcd->lp_state == PWR_OFF) {
		input_err(true, tcm_hcd->pdev->dev.parent, "power off in suspend\n");
		return -EIO;
	}

	mutex_lock(&tcm_hcd->rw_ctrl_mutex);

	if (in_buf != NULL) {
		retval = syna_tcm_raw_read(tcm_hcd, in_buf, length);
		goto exit;
	}

	retry = true;

retry:
	LOCK_BUFFER(tcm_hcd->in);

	retval = syna_tcm_read(tcm_hcd, tcm_hcd->in.buf, tcm_hcd->read_length);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to read from device\n");
		UNLOCK_BUFFER(tcm_hcd->in);
		if (retry) {
			usleep_range(READ_RETRY_US_MIN, READ_RETRY_US_MAX);
			retry = false;
			goto retry;
		}
		goto exit;
	}

	header = (struct syna_tcm_message_header *)tcm_hcd->in.buf;

	if (header->marker != MESSAGE_MARKER) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Incorrect header marker (0x%02x)\n", header->marker);
		UNLOCK_BUFFER(tcm_hcd->in);
		retval = -ENXIO;
		if (retry) {
			usleep_range(READ_RETRY_US_MIN, READ_RETRY_US_MAX);
			retry = false;
			goto retry;
		}
		goto exit;
	}

	tcm_hcd->status_report_code = header->code;

	tcm_hcd->payload_length = le2_to_uint(header->length);

	if (atomic_read(&tcm_hcd->host_downloading))
		input_info(true, tcm_hcd->pdev->dev.parent,
				"Status report code = 0x%02x\n", tcm_hcd->status_report_code);
	else
		input_dbg(false, tcm_hcd->pdev->dev.parent,
				"Status report code = 0x%02x\n", tcm_hcd->status_report_code);

	input_dbg(false, tcm_hcd->pdev->dev.parent,
				"Payload length = %d\n", tcm_hcd->payload_length);

	if (tcm_hcd->status_report_code <= STATUS_ERROR ||
		tcm_hcd->status_report_code == STATUS_INVALID) {
		switch (tcm_hcd->status_report_code) {
		case STATUS_OK:
			break;
		case STATUS_CONTINUED_READ:
			input_dbg(true, tcm_hcd->pdev->dev.parent, "Out-of-sync continued read\n");
		case STATUS_IDLE:
		case STATUS_BUSY:
			tcm_hcd->payload_length = 0;
			UNLOCK_BUFFER(tcm_hcd->in);
			retval = 0;
			goto exit;
		default:
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Incorrect Status code (0x%02x)\n", tcm_hcd->status_report_code);
			if (tcm_hcd->status_report_code == STATUS_INVALID) {
				if (retry) {
					usleep_range(READ_RETRY_US_MIN, READ_RETRY_US_MAX);
					retry = false;
					goto retry;
				} else {
					tcm_hcd->payload_length = 0;
				}
			}
		}
	}

	total_length = MESSAGE_HEADER_SIZE + tcm_hcd->payload_length + 1;

#ifdef PREDICTIVE_READING
	if (total_length <= tcm_hcd->read_length) {
		goto check_padding;
	} else if (total_length - 1 == tcm_hcd->read_length) {
		tcm_hcd->in.buf[total_length - 1] = MESSAGE_PADDING;
		goto check_padding;
	}
#else
	if (tcm_hcd->payload_length == 0) {
		tcm_hcd->in.buf[total_length - 1] = MESSAGE_PADDING;
		goto check_padding;
	}
#endif

	UNLOCK_BUFFER(tcm_hcd->in);

	retval = syna_tcm_continued_read(tcm_hcd);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do continued read\n");
		goto exit;
	};

	LOCK_BUFFER(tcm_hcd->in);

	tcm_hcd->in.buf[0] = MESSAGE_MARKER;
	tcm_hcd->in.buf[1] = tcm_hcd->status_report_code;
	tcm_hcd->in.buf[2] = (unsigned char)tcm_hcd->payload_length;
	tcm_hcd->in.buf[3] = (unsigned char)(tcm_hcd->payload_length >> 8);

check_padding:
	if (tcm_hcd->in.buf[total_length - 1] != MESSAGE_PADDING) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Incorrect message padding byte (0x%02x)\n", tcm_hcd->in.buf[total_length - 1]);
		UNLOCK_BUFFER(tcm_hcd->in);
		retval = -EIO;
		goto exit;
	}

	UNLOCK_BUFFER(tcm_hcd->in);

#ifdef PREDICTIVE_READING
	total_length = MAX(total_length, MIN_READ_LENGTH);
	tcm_hcd->read_length = MIN(total_length, tcm_hcd->rd_chunk_size);
	if (tcm_hcd->rd_chunk_size == 0)
		tcm_hcd->read_length = total_length;
#endif
	if (tcm_hcd->is_detected)
		syna_tcm_dispatch_message(tcm_hcd);

	retval = 0;

exit:
	if (retval < 0) {
		if (atomic_read(&tcm_hcd->command_status) == CMD_BUSY) {
			atomic_set(&tcm_hcd->command_status, CMD_ERROR);
			complete(&response_complete);
		}
	}

	mutex_unlock(&tcm_hcd->rw_ctrl_mutex);

	return retval;
}

/**
 * syna_tcm_write_message() - write message to device and receive response
 *
 * @tcm_hcd: handle of core module
 * @command: command to send to device
 * @payload: payload of command
 * @length: length of payload in bytes
 * @resp_buf: buffer for storing command response
 * @resp_buf_size: size of response buffer in bytes
 * @resp_length: length of command response in bytes
 * @response_code: status code returned in command response
 * @polling_delay_ms: delay time after sending command before resuming polling
 *
 * If resp_buf is NULL, raw write mode is used and syna_tcm_raw_write() is
 * called. Otherwise, a command and its payload, if any, are sent to the device
 * and the response to the command generated by the device is read in.
 */
static int syna_tcm_write_message(struct syna_tcm_hcd *tcm_hcd,
		unsigned char command, unsigned char *payload,
		unsigned int length, unsigned char **resp_buf,
		unsigned int *resp_buf_size, unsigned int *resp_length,
		unsigned char *response_code, unsigned int polling_delay_ms)
{
	int retval;
	unsigned int idx;
	unsigned int chunks;
	unsigned int chunk_space;
	unsigned int xfer_length;
	unsigned int remaining_length;
	unsigned int command_status;
	bool is_romboot_hdl = (command == CMD_ROMBOOT_DOWNLOAD) ? true : false;
	bool is_hdl_reset = (command == CMD_RESET) && (tcm_hcd->in_hdl_mode);

	if (tcm_hcd->lp_state == PWR_OFF) {
		input_err(true, tcm_hcd->pdev->dev.parent, "power off in suspend\n");
		return -EIO;
	}

	if (response_code != NULL)
		*response_code = STATUS_INVALID;

	if (!tcm_hcd->do_polling && current->pid == tcm_hcd->isr_pid) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Invalid execution context\n");
		return -EINVAL;
	}

	mutex_lock(&tcm_hcd->command_mutex);

	mutex_lock(&tcm_hcd->rw_ctrl_mutex);

	if (resp_buf == NULL) {
		retval = syna_tcm_raw_write(tcm_hcd, command, payload, length);
		mutex_unlock(&tcm_hcd->rw_ctrl_mutex);
		goto exit;
	}

	if (tcm_hcd->do_polling && polling_delay_ms) {
		cancel_delayed_work_sync(&tcm_hcd->polling_work);
		flush_workqueue(tcm_hcd->polling_workqueue);
	}

	atomic_set(&tcm_hcd->command_status, CMD_BUSY);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0))
	reinit_completion(&response_complete);
#else
	INIT_COMPLETION(response_complete);
#endif

	tcm_hcd->command = command;

	LOCK_BUFFER(tcm_hcd->resp);

	tcm_hcd->resp.buf = *resp_buf;
	tcm_hcd->resp.buf_size = *resp_buf_size;
	tcm_hcd->resp.data_length = 0;

	UNLOCK_BUFFER(tcm_hcd->resp);

	/* adding two length bytes as part of payload */
	remaining_length = length + 2;

	/* available chunk space for payload = total chunk size minus command
	 * byte */
	if (tcm_hcd->wr_chunk_size == 0)
		chunk_space = remaining_length;
	else
		chunk_space = tcm_hcd->wr_chunk_size - 1;

	if (is_romboot_hdl) {
		if (HDL_WR_CHUNK_SIZE) {
			chunk_space = HDL_WR_CHUNK_SIZE - 1;
			chunk_space = chunk_space - (chunk_space % ROMBOOT_DOWNLOAD_UNIT);
		} else {
			chunk_space = remaining_length;
		}
	}

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	input_info(true, tcm_hcd->pdev->dev.parent, "Command = 0x%02x\n", command);

	LOCK_BUFFER(tcm_hcd->out);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space)
			xfer_length = chunk_space;
		else
			xfer_length = remaining_length;

		retval = syna_tcm_alloc_mem(tcm_hcd, &tcm_hcd->out, xfer_length + 1);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to allocate memory for tcm_hcd->out.buf\n");
			UNLOCK_BUFFER(tcm_hcd->out);
			mutex_unlock(&tcm_hcd->rw_ctrl_mutex);
			goto exit;
		}

		if (idx == 0) {
			tcm_hcd->out.buf[0] = command;
			tcm_hcd->out.buf[1] = (unsigned char)length;
			tcm_hcd->out.buf[2] = (unsigned char)(length >> 8);

			if (xfer_length > 2) {
				retval = secure_memcpy(&tcm_hcd->out.buf[3],
							tcm_hcd->out.buf_size - 3, payload,
							remaining_length - 2, xfer_length - 2);
				if (retval < 0) {
					input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy payload\n");
					UNLOCK_BUFFER(tcm_hcd->out);
					mutex_unlock(&tcm_hcd->rw_ctrl_mutex);
					goto exit;
				}
			}
		} else {
			tcm_hcd->out.buf[0] = CMD_CONTINUE_WRITE;

			retval = secure_memcpy(&tcm_hcd->out.buf[1],
						tcm_hcd->out.buf_size - 1, &payload[idx * chunk_space - 2],
						remaining_length, xfer_length);
			if (retval < 0) {
				input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy payload\n");
				UNLOCK_BUFFER(tcm_hcd->out);
				mutex_unlock(&tcm_hcd->rw_ctrl_mutex);
				goto exit;
			}
		}

		retval = syna_tcm_write(tcm_hcd, tcm_hcd->out.buf, xfer_length + 1);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to write to device\n");
			UNLOCK_BUFFER(tcm_hcd->out);
			mutex_unlock(&tcm_hcd->rw_ctrl_mutex);
			goto exit;
		}

		remaining_length -= xfer_length;

		if (chunks > 1)
			usleep_range(WRITE_DELAY_US_MIN, WRITE_DELAY_US_MAX);
	}

	UNLOCK_BUFFER(tcm_hcd->out);

	mutex_unlock(&tcm_hcd->rw_ctrl_mutex);

	if (is_hdl_reset)
		goto exit;

	if (tcm_hcd->do_polling && polling_delay_ms) {
		queue_delayed_work(tcm_hcd->polling_workqueue, &tcm_hcd->polling_work,
			msecs_to_jiffies(polling_delay_ms));
	}

	retval = wait_for_completion_timeout(&response_complete,
					msecs_to_jiffies(RESPONSE_TIMEOUT_MS));
	if (retval == 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Timed out waiting for response (command 0x%02x)\n", tcm_hcd->command);
		retval = -ETIME;
		goto exit;
	}

	command_status = atomic_read(&tcm_hcd->command_status);
	if (command_status != CMD_IDLE) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to get valid response (command 0x%02x)\n", tcm_hcd->command);
		retval = -EIO;
		goto exit;
	}

	LOCK_BUFFER(tcm_hcd->resp);

	if (tcm_hcd->response_code != STATUS_OK) {
		if (tcm_hcd->resp.data_length) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Error code = 0x%02x (command 0x%02x)\n",
				tcm_hcd->resp.buf[0],tcm_hcd->command);
		}
		retval = -EIO;
	} else {
		retval = 0;
	}

	*resp_buf = tcm_hcd->resp.buf;
	*resp_buf_size = tcm_hcd->resp.buf_size;
	*resp_length = tcm_hcd->resp.data_length;

	if (response_code != NULL)
		*response_code = tcm_hcd->response_code;

	UNLOCK_BUFFER(tcm_hcd->resp);

exit:
	tcm_hcd->command = CMD_NONE;

	atomic_set(&tcm_hcd->command_status, CMD_IDLE);

	mutex_unlock(&tcm_hcd->command_mutex);

	return retval;
}

static int syna_tcm_wait_hdl(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;

	msleep(HOST_DOWNLOAD_WAIT_MS);

	if (!atomic_read(&tcm_hcd->host_downloading))
		return 0;

	retval = wait_event_interruptible_timeout(tcm_hcd->hdl_wq,
		!atomic_read(&tcm_hcd->host_downloading), msecs_to_jiffies(HOST_DOWNLOAD_TIMEOUT_MS));
	if (retval == 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Timed out waiting for completion of host download\n");
		atomic_set(&tcm_hcd->host_downloading, 0);
		retval = -EIO;
	} else {
		retval = 0;
	}

	return retval;
}

static void syna_tcm_check_hdl(struct syna_tcm_hcd *tcm_hcd, unsigned char id)
{
	struct syna_tcm_module_handler *mod_handler;

	LOCK_BUFFER(tcm_hcd->report.buffer);

	tcm_hcd->report.buffer.buf = NULL;
	tcm_hcd->report.buffer.buf_size = 0;
	tcm_hcd->report.buffer.data_length = 0;
	tcm_hcd->report.id = id;

	UNLOCK_BUFFER(tcm_hcd->report.buffer);

	mutex_lock(&mod_pool.mutex);

	if (!list_empty(&mod_pool.list)) {
		list_for_each_entry(mod_handler, &mod_pool.list, link) {
			if (!mod_handler->insert && !mod_handler->detach && mod_handler->mod_cb->syncbox)
				mod_handler->mod_cb->syncbox(tcm_hcd);
		}
	}

	mutex_unlock(&mod_pool.mutex);

	return;
}

#ifdef WATCHDOG_SW
static void syna_tcm_update_watchdog(struct syna_tcm_hcd *tcm_hcd, bool en)
{
	cancel_delayed_work_sync(&tcm_hcd->watchdog.work);
	flush_workqueue(tcm_hcd->watchdog.workqueue);

	if (!tcm_hcd->watchdog.run) {
		tcm_hcd->watchdog.count = 0;
		return;
	}

	if (en) {
		queue_delayed_work(tcm_hcd->watchdog.workqueue,	&tcm_hcd->watchdog.work,
			msecs_to_jiffies(WATCHDOG_DELAY_MS));
	} else {
		tcm_hcd->watchdog.count = 0;
	}

	return;
}

static void syna_tcm_watchdog_work(struct work_struct *work)
{
	int retval;
	unsigned char marker;
	struct delayed_work *delayed_work = container_of(work, struct delayed_work, work);
	struct syna_tcm_watchdog *watchdog =
								container_of(delayed_work, struct syna_tcm_watchdog, work);
	struct syna_tcm_hcd *tcm_hcd = container_of(watchdog, struct syna_tcm_hcd, watchdog);

	if (mutex_is_locked(&tcm_hcd->rw_ctrl_mutex))
		goto exit;

	mutex_lock(&tcm_hcd->rw_ctrl_mutex);

	retval = syna_tcm_read(tcm_hcd, &marker, 1);

	mutex_unlock(&tcm_hcd->rw_ctrl_mutex);

	if (retval < 0 || marker != MESSAGE_MARKER) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to read from device\n");

		tcm_hcd->watchdog.count++;

		if (tcm_hcd->watchdog.count >= WATCHDOG_TRIGGER_COUNT) {
			retval = tcm_hcd->reset_n_reinit(tcm_hcd, true, false);
			if (retval < 0) {
				input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do reset and reinit\n");
			}
			tcm_hcd->watchdog.count = 0;
		}
	}

exit:
	queue_delayed_work(tcm_hcd->watchdog.workqueue, &tcm_hcd->watchdog.work,
			msecs_to_jiffies(WATCHDOG_DELAY_MS));

	return;
}
#endif

static void syna_tcm_polling_work(struct work_struct *work)
{
	int retval;
	struct delayed_work *delayed_work = container_of(work, struct delayed_work, work);
	struct syna_tcm_hcd *tcm_hcd =
						container_of(delayed_work, struct syna_tcm_hcd, polling_work);

	if (!tcm_hcd->do_polling)
		return;

	retval = tcm_hcd->read_message(tcm_hcd, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "%s Failed to read message\n", __func__);
		if (retval == -ENXIO && tcm_hcd->hw_if->bus_io->type == BUS_SPI)
			syna_tcm_check_hdl(tcm_hcd, REPORT_HDL_F35);
	}

	if ((tcm_hcd->lp_state == PWR_ON) || (retval >= 0)) {
		queue_delayed_work(tcm_hcd->polling_workqueue, &tcm_hcd->polling_work,
				msecs_to_jiffies(POLLING_DELAY_MS));
	}

	return;
}

static irqreturn_t syna_tcm_isr(int irq, void *data)
{
	int retval;
	struct syna_tcm_hcd *tcm_hcd = data;
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;

	if (unlikely(gpio_get_value(bdata->irq_gpio) != bdata->irq_on_state))
		goto exit;

	tcm_hcd->isr_pid = current->pid;

	retval = tcm_hcd->read_message(tcm_hcd, NULL, 0);
	if (retval < 0) {
		if (tcm_hcd->sensor_type == TYPE_F35)
			syna_tcm_check_hdl(tcm_hcd, REPORT_HDL_F35);
		else
			input_err(true, tcm_hcd->pdev->dev.parent, "%s Failed to read message\n", __func__);
	}

exit:
	return IRQ_HANDLED;
}

static int syna_tcm_enable_irq(struct syna_tcm_hcd *tcm_hcd, bool en, bool ns)
{
	int retval;
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;
	static bool irq_freed = true;

	mutex_lock(&tcm_hcd->irq_en_mutex);

	if (en) {
		if (tcm_hcd->irq_enabled) {
			input_dbg(true, tcm_hcd->pdev->dev.parent, "Interrupt already enabled\n");
			retval = 0;
			goto exit;
		}

		if (bdata->irq_gpio < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Invalid IRQ GPIO\n");
			retval = -EINVAL;
			goto queue_polling_work;
		}

		if (irq_freed) {
			retval = request_threaded_irq(tcm_hcd->irq, NULL, syna_tcm_isr, bdata->irq_flags,
							PLATFORM_DRIVER_NAME, tcm_hcd);
			if (retval < 0) {
				input_err(true, tcm_hcd->pdev->dev.parent,
					"Failed to create interrupt thread\n");
			}
		} else {
			enable_irq(tcm_hcd->irq);
			retval = 0;
		}

queue_polling_work:
		if (retval < 0) {
#ifdef FALL_BACK_ON_POLLING
			queue_delayed_work(tcm_hcd->polling_workqueue, &tcm_hcd->polling_work,
				msecs_to_jiffies(POLLING_DELAY_MS));
			tcm_hcd->do_polling = true;
			retval = 0;
#endif
		}

		if (retval < 0)
			goto exit;
		else
			msleep(ENABLE_IRQ_DELAY_MS);
	} else {
		if (!tcm_hcd->irq_enabled) {
			input_dbg(true, tcm_hcd->pdev->dev.parent, "Interrupt already disabled\n");
			retval = 0;
			goto exit;
		}

		if (bdata->irq_gpio >= 0) {
			if (ns) {
				disable_irq_nosync(tcm_hcd->irq);
			} else {
				disable_irq(tcm_hcd->irq);
				free_irq(tcm_hcd->irq, tcm_hcd);
			}
			irq_freed = !ns;
		}

		if (ns) {
			cancel_delayed_work(&tcm_hcd->polling_work);
		} else {
			cancel_delayed_work_sync(&tcm_hcd->polling_work);
			flush_workqueue(tcm_hcd->polling_workqueue);
		}

		tcm_hcd->do_polling = false;
	}

	retval = 0;

exit:
	if (retval == 0)
		tcm_hcd->irq_enabled = en;

	mutex_unlock(&tcm_hcd->irq_en_mutex);

	return retval;
}

static int syna_tcm_set_gpio(struct syna_tcm_hcd *tcm_hcd, int gpio,
		bool config, int dir, int state)
{
	int retval;
	char label[16];

	if (config) {
		retval = snprintf(label, 16, "tcm_gpio_%d\n", gpio);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to set GPIO label\n");
			return retval;
		}

		retval = gpio_request(gpio, label);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to request GPIO %d\n", gpio);
			return retval;
		}

		if (dir == 0)
			retval = gpio_direction_input(gpio);
		else
			retval = gpio_direction_output(gpio, state);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to set GPIO %d direction\n", gpio);
			return retval;
		}
	} else {
		gpio_free(gpio);
	}

	return 0;
}

static int syna_tcm_config_gpio(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;

	if (bdata->irq_gpio >= 0) {
		retval = syna_tcm_set_gpio(tcm_hcd, bdata->irq_gpio, true, 0, 0);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to configure interrupt GPIO\n");
			goto err_set_gpio_irq;
		}
	}

	if (bdata->cs_gpio >= 0) {
		retval = syna_tcm_set_gpio(tcm_hcd, bdata->cs_gpio, true, 1, 0);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to configure interrupt GPIO\n");
			goto err_set_gpio_cs;
		}
	}

	if (bdata->power_gpio >= 0) {
		retval = syna_tcm_set_gpio(tcm_hcd, bdata->power_gpio, true, 1, !bdata->power_on_state);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to configure power GPIO\n");
			goto err_set_gpio_power;
		}
	}

	if (bdata->reset_gpio >= 0) {
		retval = syna_tcm_set_gpio(tcm_hcd, bdata->reset_gpio, true, 1, !bdata->reset_on_state);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to configure reset GPIO\n");
			goto err_set_gpio_reset;
		}
	}

	if (bdata->power_gpio >= 0) {
		gpio_set_value(bdata->power_gpio, bdata->power_on_state);
		msleep(bdata->power_delay_ms);
	}

	if (bdata->reset_gpio >= 0) {
		gpio_set_value(bdata->reset_gpio, bdata->reset_on_state);
		msleep(bdata->reset_active_ms);
		gpio_set_value(bdata->reset_gpio, !bdata->reset_on_state);
		msleep(bdata->reset_delay_ms);
	}

	return 0;

err_set_gpio_reset:
	if (bdata->power_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->power_gpio, false, 0, 0);
err_set_gpio_power:
	if (bdata->irq_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->cs_gpio, false, 0, 0);
err_set_gpio_cs:
	if (bdata->irq_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->irq_gpio, false, 0, 0);
err_set_gpio_irq:
	return retval;
}

static int syna_tcm_enable_regulator(struct syna_tcm_hcd *tcm_hcd, bool en)
{
	int retval;
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;

	if (!en) {
		retval = 0;
		goto disable_pwr_reg;
	}

	if (tcm_hcd->bus_reg) {
		retval = regulator_enable(tcm_hcd->bus_reg);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to enable bus regulator\n");
			goto exit;
		}
	}

	if (tcm_hcd->pwr_reg) {
		retval = regulator_enable(tcm_hcd->pwr_reg);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to enable power regulator\n");
			goto disable_bus_reg;
		}
		msleep(bdata->power_delay_ms);
	}

	return 0;

disable_pwr_reg:
	if (tcm_hcd->pwr_reg)
		regulator_disable(tcm_hcd->pwr_reg);

disable_bus_reg:
	if (tcm_hcd->bus_reg)
		regulator_disable(tcm_hcd->bus_reg);

exit:
	return retval;
}

static int syna_tcm_get_regulator(struct syna_tcm_hcd *tcm_hcd, bool get)
{
	int retval;
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;

	if (!get) {
		retval = 0;
		goto regulator_put;
	}

	if (bdata->bus_reg_name != NULL && *bdata->bus_reg_name != 0) {
		tcm_hcd->bus_reg = regulator_get(tcm_hcd->pdev->dev.parent, bdata->bus_reg_name);
		if (IS_ERR(tcm_hcd->bus_reg)) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to get bus regulator\n");
			retval = PTR_ERR(tcm_hcd->bus_reg);
			goto regulator_put;
		}
	}

	if (bdata->pwr_reg_name != NULL && *bdata->pwr_reg_name != 0) {
		tcm_hcd->pwr_reg = regulator_get(tcm_hcd->pdev->dev.parent, bdata->pwr_reg_name);
		if (IS_ERR(tcm_hcd->pwr_reg)) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to get power regulator\n");
			retval = PTR_ERR(tcm_hcd->pwr_reg);
			goto regulator_put;
		}
	}

	return 0;

regulator_put:
	if (tcm_hcd->bus_reg) {
		regulator_put(tcm_hcd->bus_reg);
		tcm_hcd->bus_reg = NULL;
	}

	if (tcm_hcd->pwr_reg) {
		regulator_put(tcm_hcd->pwr_reg);
		tcm_hcd->pwr_reg = NULL;
	}

	return retval;
}

int syna_tcm_get_app_info(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;
	unsigned int timeout;

	timeout = APP_STATUS_POLL_TIMEOUT_MS;

	resp_buf = NULL;
	resp_buf_size = 0;

get_app_info:
	retval = tcm_hcd->write_message(tcm_hcd, CMD_GET_APPLICATION_INFO,
				NULL, 0, &resp_buf,	&resp_buf_size, &resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_GET_APPLICATION_INFO));
		goto exit;
	}

	retval = secure_memcpy((unsigned char *)&tcm_hcd->app_info,
				sizeof(tcm_hcd->app_info), resp_buf, resp_buf_size,
				MIN(sizeof(tcm_hcd->app_info), resp_length));
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy application info\n");
		goto exit;
	}

	tcm_hcd->app_status = le2_to_uint(tcm_hcd->app_info.status);

	if (tcm_hcd->app_status == APP_STATUS_BOOTING ||
				tcm_hcd->app_status == APP_STATUS_UPDATING) {
		if (timeout > 0) {
			msleep(APP_STATUS_POLL_MS);
			timeout -= APP_STATUS_POLL_MS;
			goto get_app_info;
		}
	}

	input_info(true, tcm_hcd->pdev->dev.parent, "config version %02X%02X%02X%02X\n",
		tcm_hcd->app_info.customer_config_id[0], tcm_hcd->app_info.customer_config_id[1],
		tcm_hcd->app_info.customer_config_id[2], tcm_hcd->app_info.customer_config_id[3]);

	tcm_hcd->cols = le2_to_uint(tcm_hcd->app_info.num_of_image_cols);
	tcm_hcd->rows = le2_to_uint(tcm_hcd->app_info.num_of_image_rows);

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_get_boot_info(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	resp_buf = NULL;
	resp_buf_size = 0;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_GET_BOOT_INFO, NULL,
				0, &resp_buf, &resp_buf_size, &resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to write command %s\n", STR(CMD_GET_BOOT_INFO));
		goto exit;
	}

	retval = secure_memcpy((unsigned char *)&tcm_hcd->boot_info,
				sizeof(tcm_hcd->boot_info), resp_buf, resp_buf_size,
				MIN(sizeof(tcm_hcd->boot_info), resp_length));
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy boot info\n");
		goto exit;
	}

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_get_romboot_info(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	resp_buf = NULL;
	resp_buf_size = 0;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_GET_ROMBOOT_INFO,
				NULL, 0, &resp_buf, &resp_buf_size, &resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
					"Failed to write command %s\n", STR(CMD_GET_ROMBOOT_INFO));
		goto exit;
	}

	retval = secure_memcpy((unsigned char *)&tcm_hcd->romboot_info,
				sizeof(tcm_hcd->romboot_info), resp_buf, resp_buf_size,
				MIN(sizeof(tcm_hcd->romboot_info), resp_length));
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy boot info\n");
		goto exit;
	}

	input_dbg(true, tcm_hcd->pdev->dev.parent,
		"version = %d\n", tcm_hcd->romboot_info.version);

	input_dbg(true, tcm_hcd->pdev->dev.parent,
		 "status = 0x%02x\n", tcm_hcd->romboot_info.status);

	input_dbg(true, tcm_hcd->pdev->dev.parent,
		"version = 0x%02x 0x%02x\n", tcm_hcd->romboot_info.asic_id[0],
		tcm_hcd->romboot_info.asic_id[1]);

	input_dbg(true, tcm_hcd->pdev->dev.parent,
		"write_block_size_words = %d\n", tcm_hcd->romboot_info.write_block_size_words);

	input_dbg(true, tcm_hcd->pdev->dev.parent,
		"max_write_payload_size = %d\n",
		tcm_hcd->romboot_info.max_write_payload_size[0] |
		tcm_hcd->romboot_info.max_write_payload_size[1] << 8);

	input_dbg(true, tcm_hcd->pdev->dev.parent,
		"last_reset_reason = 0x%02x\n", tcm_hcd->romboot_info.last_reset_reason);

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_identify(struct syna_tcm_hcd *tcm_hcd, bool id)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;
	unsigned int max_write_size;

	resp_buf = NULL;
	resp_buf_size = 0;

	mutex_lock(&tcm_hcd->identify_mutex);

	if (!id)
		goto get_info;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_IDENTIFY,
				NULL, 0, &resp_buf, &resp_buf_size, &resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_IDENTIFY));
		goto exit;
	}

	retval = secure_memcpy((unsigned char *)&tcm_hcd->id_info,
				sizeof(tcm_hcd->id_info), resp_buf, resp_buf_size,
				MIN(sizeof(tcm_hcd->id_info), resp_length));
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy identification info\n");
		goto exit;
	}

	tcm_hcd->packrat_number = le4_to_uint(tcm_hcd->id_info.build_id);

	max_write_size = le2_to_uint(tcm_hcd->id_info.max_write_size);
	tcm_hcd->wr_chunk_size = MIN(max_write_size, WR_CHUNK_SIZE);
	if (tcm_hcd->wr_chunk_size == 0)
		tcm_hcd->wr_chunk_size = max_write_size;

	input_info(true, tcm_hcd->pdev->dev.parent,
		"Firmware build id = %d\n", tcm_hcd->packrat_number);

get_info:
	switch (tcm_hcd->id_info.mode) {
	case MODE_APPLICATION_FIRMWARE:
	case MODE_HOSTDOWNLOAD_FIRMWARE:

		retval = syna_tcm_get_app_info(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to get application info\n");
			goto exit;
		}
		break;
	case MODE_BOOTLOADER:
	case MODE_TDDI_BOOTLOADER:

		input_dbg(true, tcm_hcd->pdev->dev.parent, "In bootloader mode\n");

		retval = syna_tcm_get_boot_info(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to get boot info\n");
			goto exit;
		}
		break;
	case MODE_ROMBOOTLOADER:

		input_dbg(true, tcm_hcd->pdev->dev.parent, "In rombootloader mode\n");

		retval = syna_tcm_get_romboot_info(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to get application info\n");
			goto exit;
		}
		break;
	default:
		break;
	}

	retval = 0;

exit:
	mutex_unlock(&tcm_hcd->identify_mutex);

	kfree(resp_buf);

	return retval;
}

static int syna_tcm_run_production_test_firmware(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	bool retry;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	retry = true;

	resp_buf = NULL;
	resp_buf_size = 0;

retry:
	retval = tcm_hcd->write_message(tcm_hcd, CMD_ENTER_PRODUCTION_TEST_MODE,
				NULL, 0, &resp_buf, &resp_buf_size, &resp_length,
				NULL, MODE_SWITCH_DELAY_MS);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_ENTER_PRODUCTION_TEST_MODE));
		goto exit;
	}

	if (tcm_hcd->id_info.mode != MODE_PRODUCTIONTEST_FIRMWARE) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to run production test firmware\n");
		if (retry) {
			retry = false;
			goto retry;
		}
		retval = -EINVAL;
		goto exit;
	} else if (tcm_hcd->app_status != APP_STATUS_OK) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Application status = 0x%02x\n", tcm_hcd->app_status);
	}

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_run_application_firmware(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	bool retry;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	retry = true;

	resp_buf = NULL;
	resp_buf_size = 0;

retry:
	retval = tcm_hcd->write_message(tcm_hcd, CMD_RUN_APPLICATION_FIRMWARE,
				NULL, 0, &resp_buf, &resp_buf_size, &resp_length,
				NULL, MODE_SWITCH_DELAY_MS);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_RUN_APPLICATION_FIRMWARE));
		goto exit;
	}

	retval = tcm_hcd->identify(tcm_hcd, false);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do identification\n");
		goto exit;
	}

	if (IS_NOT_FW_MODE(tcm_hcd->id_info.mode)) {
		input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to run application firmware (boot status = 0x%02x)\n",
				tcm_hcd->boot_info.status);
		if (retry) {
			retry = false;
			goto retry;
		}
		retval = -EINVAL;
		goto exit;
	} else if (tcm_hcd->app_status != APP_STATUS_OK) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Application status = 0x%02x\n", tcm_hcd->app_status);
	}

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_run_bootloader_firmware(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;
	unsigned char command;

	resp_buf = NULL;
	resp_buf_size = 0;
	command = (tcm_hcd->id_info.mode == MODE_ROMBOOTLOADER) ?
				CMD_ROMBOOT_RUN_BOOTLOADER_FIRMWARE : CMD_RUN_BOOTLOADER_FIRMWARE;

	retval = tcm_hcd->write_message(tcm_hcd, command,
				NULL, 0, &resp_buf, &resp_buf_size,
				&resp_length, NULL, MODE_SWITCH_DELAY_MS);
	if (retval < 0) {
		if (tcm_hcd->id_info.mode == MODE_ROMBOOTLOADER) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to write command %s\n", STR(CMD_ROMBOOT_RUN_BOOTLOADER_FIRMWARE));
		} else {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to write command %s\n", STR(CMD_RUN_BOOTLOADER_FIRMWARE));
		}
		goto exit;
	}

	if (command != CMD_ROMBOOT_RUN_BOOTLOADER_FIRMWARE) {
		retval = tcm_hcd->identify(tcm_hcd, false);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do identification\n");
		goto exit;
		}

		if (IS_FW_MODE(tcm_hcd->id_info.mode)) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to enter bootloader mode\n");
			retval = -EINVAL;
			goto exit;
		}
	}

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_switch_mode(struct syna_tcm_hcd *tcm_hcd,
		enum firmware_mode mode)
{
	int retval;

	mutex_lock(&tcm_hcd->reset_mutex);

#ifdef WATCHDOG_SW
	tcm_hcd->update_watchdog(tcm_hcd, false);
#endif

	switch (mode) {
	case FW_MODE_BOOTLOADER:
		retval = syna_tcm_run_bootloader_firmware(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to switch to bootloader mode\n");
			goto exit;
		}
		break;
	case FW_MODE_APPLICATION:
		retval = syna_tcm_run_application_firmware(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to switch to application mode\n");
			goto exit;
		}
		break;
	case FW_MODE_PRODUCTION_TEST:
		retval = syna_tcm_run_production_test_firmware(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to switch to production test mode\n");
			goto exit;
		}
		break;
	default:
		input_err(true, tcm_hcd->pdev->dev.parent, "Invalid firmware mode\n");
		retval = -EINVAL;
		goto exit;
	}

	retval = 0;

exit:
#ifdef WATCHDOG_SW
	tcm_hcd->update_watchdog(tcm_hcd, true);
#endif

	mutex_unlock(&tcm_hcd->reset_mutex);

	return retval;
}

static int syna_tcm_get_dynamic_config(struct syna_tcm_hcd *tcm_hcd,
		enum dynamic_config_id id, unsigned short *value)
{
	int retval;
	unsigned char out_buf;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	resp_buf = NULL;
	resp_buf_size = 0;

	out_buf = (unsigned char)id;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_GET_DYNAMIC_CONFIG,
				&out_buf, sizeof(out_buf), &resp_buf,
				&resp_buf_size, &resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_GET_DYNAMIC_CONFIG));
		goto exit;
	}

	if (resp_length < 2) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Invalid data length\n");
		retval = -EINVAL;
		goto exit;
	}

	*value = (unsigned short)le2_to_uint(resp_buf);

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_set_dynamic_config(struct syna_tcm_hcd *tcm_hcd,
		enum dynamic_config_id id, unsigned short value)
{
	int retval;
	unsigned char out_buf[3];
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	resp_buf = NULL;
	resp_buf_size = 0;

	out_buf[0] = (unsigned char)id;
	out_buf[1] = (unsigned char)value;
	out_buf[2] = (unsigned char)(value >> 8);

	retval = tcm_hcd->write_message(tcm_hcd, CMD_SET_DYNAMIC_CONFIG,
				out_buf, sizeof(out_buf), &resp_buf,
				&resp_buf_size, &resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_SET_DYNAMIC_CONFIG));
		goto exit;
	}

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_get_data_location(struct syna_tcm_hcd *tcm_hcd,
		enum flash_area area, unsigned int *addr, unsigned int *length)
{
	int retval;
	unsigned char out_buf;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	switch (area) {
	case CUSTOM_LCM:
		out_buf = LCM_DATA;
		break;
	case CUSTOM_OEM:
		out_buf = OEM_DATA;
		break;
	case PPDT:
		out_buf = PPDT_DATA;
		break;
	default:
		input_err(true, tcm_hcd->pdev->dev.parent, "Invalid flash area\n");
		return -EINVAL;
	}

	resp_buf = NULL;
	resp_buf_size = 0;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_GET_DATA_LOCATION,
				&out_buf, sizeof(out_buf), &resp_buf,
				&resp_buf_size, &resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_GET_DATA_LOCATION));
		goto exit;
	}

	if (resp_length != 4) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Invalid data length\n");
		retval = -EINVAL;
		goto exit;
	}

	*addr = le2_to_uint(&resp_buf[0]);
	*length = le2_to_uint(&resp_buf[2]);

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_sleep(struct syna_tcm_hcd *tcm_hcd, bool en)
{
	int retval;
	unsigned char command;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	command = en ? CMD_ENTER_DEEP_SLEEP : CMD_EXIT_DEEP_SLEEP;

	resp_buf = NULL;
	resp_buf_size = 0;

	retval = tcm_hcd->write_message(tcm_hcd, command,
				NULL, 0, &resp_buf, &resp_buf_size,
				&resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n",
			en ? STR(CMD_ENTER_DEEP_SLEEP) : STR(CMD_EXIT_DEEP_SLEEP));
		goto exit;
	}

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}

static int syna_tcm_reset(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_RESET,
				NULL, 0, &resp_buf, &resp_buf_size,
				&resp_length, NULL, bdata->reset_delay_ms);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_RESET));
	}

	return retval;
}

static int syna_tcm_reset_and_reinit(struct syna_tcm_hcd *tcm_hcd,
		bool hw, bool update_wd)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;
	struct syna_tcm_module_handler *mod_handler;
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;

	resp_buf = NULL;
	resp_buf_size = 0;

	mutex_lock(&tcm_hcd->reset_mutex);

#ifdef WATCHDOG_SW
	if (update_wd)
		tcm_hcd->update_watchdog(tcm_hcd, false);
#endif

	if (hw) {
		if (bdata->reset_gpio < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Hardware reset unavailable\n");
			retval = -EINVAL;
			goto exit;
		}
		gpio_set_value(bdata->reset_gpio, bdata->reset_on_state);
		msleep(bdata->reset_active_ms);
		gpio_set_value(bdata->reset_gpio, !bdata->reset_on_state);
	} else {
		retval = syna_tcm_reset(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do reset\n");
			goto exit;
		}
	}

	/* for hdl, the remaining re-init process will be done */
	/* in the helper thread, so wait for the completion here */
	if (tcm_hcd->in_hdl_mode) {
		mutex_unlock(&tcm_hcd->reset_mutex);
		kfree(resp_buf);

		retval = syna_tcm_wait_hdl(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to wait for completion of host download\n");
			return retval;
		}

#ifdef WATCHDOG_SW
		if (update_wd)
			tcm_hcd->update_watchdog(tcm_hcd, true);
#endif
		return 0;
	}

	msleep(bdata->reset_delay_ms);

	retval = tcm_hcd->identify(tcm_hcd, false);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do identification\n");
		goto exit;
	}

	if (IS_FW_MODE(tcm_hcd->id_info.mode))
		goto get_features;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_RUN_APPLICATION_FIRMWARE,
				NULL, 0, &resp_buf, &resp_buf_size,
				&resp_length, NULL, MODE_SWITCH_DELAY_MS);
	if (retval < 0) {
		input_info(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_RUN_APPLICATION_FIRMWARE));
	}

	retval = tcm_hcd->identify(tcm_hcd, false);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do identification\n");
		goto exit;
	}

get_features:
	input_info(true, tcm_hcd->pdev->dev.parent,
		"Firmware mode = 0x%02x\n", tcm_hcd->id_info.mode);

	if (IS_NOT_FW_MODE(tcm_hcd->id_info.mode)) {
		input_info(true, tcm_hcd->pdev->dev.parent,
			"Boot status = 0x%02x\n", tcm_hcd->boot_info.status);
	} else if (tcm_hcd->app_status != APP_STATUS_OK) {
		input_info(true, tcm_hcd->pdev->dev.parent,
			"Application status = 0x%02x\n", tcm_hcd->app_status);
	}

	if (IS_NOT_FW_MODE(tcm_hcd->id_info.mode))
		goto dispatch_reinit;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_GET_FEATURES,
				NULL, 0, &resp_buf, &resp_buf_size,
				&resp_length, NULL, 0);
	if (retval < 0) {
		input_info(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_GET_FEATURES));
	} else {
		retval = secure_memcpy((unsigned char *)&tcm_hcd->features,
					sizeof(tcm_hcd->features), resp_buf, resp_buf_size,
					MIN(sizeof(tcm_hcd->features), resp_length));
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy feature description\n");
		}
	}

dispatch_reinit:
	mutex_lock(&mod_pool.mutex);

	if (!list_empty(&mod_pool.list)) {
		list_for_each_entry(mod_handler, &mod_pool.list, link) {
			if (!mod_handler->insert && !mod_handler->detach && (mod_handler->mod_cb->reinit))
				mod_handler->mod_cb->reinit(tcm_hcd);
		}
	}

	mutex_unlock(&mod_pool.mutex);

	retval = 0;

exit:
#ifdef WATCHDOG_SW
	if (update_wd)
		tcm_hcd->update_watchdog(tcm_hcd, true);
#endif

	mutex_unlock(&tcm_hcd->reset_mutex);

	kfree(resp_buf);

	return retval;
}

#ifdef USE_FLASH
static int syna_tcm_rezero(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	resp_buf = NULL;
	resp_buf_size = 0;

	retval = tcm_hcd->write_message(tcm_hcd, CMD_REZERO,
				NULL, 0, &resp_buf, &resp_buf_size,
				&resp_length, NULL, 0);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to write command %s\n", STR(CMD_REZERO));
		goto exit;
	}

	retval = 0;

exit:
	kfree(resp_buf);

	return retval;
}
#endif
static void syna_tcm_helper_work(struct work_struct *work)
{
	int retval;
	unsigned char task;
	struct syna_tcm_module_handler *mod_handler;
	struct syna_tcm_helper *helper = container_of(work, struct syna_tcm_helper, work);
	struct syna_tcm_hcd *tcm_hcd = container_of(helper, struct syna_tcm_hcd, helper);

	task = atomic_read(&helper->task);

	switch (task) {

	/* this helper can help to run the application firmware */
	case HELP_RUN_APPLICATION_FIRMWARE:
		mutex_lock(&tcm_hcd->reset_mutex);

#ifdef WATCHDOG_SW
		tcm_hcd->update_watchdog(tcm_hcd, false);
#endif
		retval = syna_tcm_run_application_firmware(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to switch to application mode\n");
		}
#ifdef WATCHDOG_SW
		tcm_hcd->update_watchdog(tcm_hcd, true);
#endif
		mutex_unlock(&tcm_hcd->reset_mutex);
		break;

	/* the reinit helper is used to notify all installed modules to */
	/* do the re-initialization process, since the HDL is completed */
	case HELP_SEND_REINIT_NOTIFICATION:
		mutex_lock(&tcm_hcd->reset_mutex);

		/* do identify to ensure application firmware is running */
		retval = tcm_hcd->identify(tcm_hcd, true);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Application firmware is not running\n");
			mutex_unlock(&tcm_hcd->reset_mutex);
			break;
		}

		/* init the touch reporting here */
		/* since the HDL is completed */
		retval = touch_reinit(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to initialze touch reporting\n");
			mutex_unlock(&tcm_hcd->reset_mutex);
			break;
		}

		mutex_lock(&mod_pool.mutex);
		if (!list_empty(&mod_pool.list)) {
			list_for_each_entry(mod_handler, &mod_pool.list, link) {
				if (!mod_handler->insert &&	!mod_handler->detach && (mod_handler->mod_cb->reinit))
					mod_handler->mod_cb->reinit(tcm_hcd);
			}
		}
		mutex_unlock(&mod_pool.mutex);
		mutex_unlock(&tcm_hcd->reset_mutex);
		wake_up_interruptible(&tcm_hcd->hdl_wq);
		break;

	/* this helper is used to reinit the touch reporting */
	case HELP_TOUCH_REINIT:
		retval = touch_reinit(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent,
				"Failed to re-initialze touch reporting\n");
		}
		break;

	/* this helper is used to trigger a romboot hdl */
	case HELP_SEND_ROMBOOT_HDL:
		syna_tcm_check_hdl(tcm_hcd, REPORT_HDL_ROMBOOT);
		break;
	default:
		break;
	}

	atomic_set(&helper->task, HELP_NONE);

	return;
}

static int pinctrl_configure(struct syna_tcm_hcd *tcm_hcd, bool enable)
{
	struct pinctrl_state *state;

	input_dbg(true, tcm_hcd->pdev->dev.parent, "%s: %s\n", __func__,
									enable ? "ACTIVE" : "SUSPEND");

	if (enable) {
		state = pinctrl_lookup_state(tcm_hcd->pinctrl, "on_state");
		if (IS_ERR(tcm_hcd->pinctrl))
			input_err(true, tcm_hcd->pdev->dev.parent,
				"%s: could not get active pinstate\n", __func__);
	} else {
		state = pinctrl_lookup_state(tcm_hcd->pinctrl, "off_state");
		if (IS_ERR(tcm_hcd->pinctrl))
			input_err(true, tcm_hcd->pdev->dev.parent,
				"%s: could not get suspend pinstate\n", __func__);
	}

	if (!IS_ERR_OR_NULL(state))
		return pinctrl_select_state(tcm_hcd->pinctrl, state);

	return 0;
}

#if defined(CONFIG_PM) || defined(CONFIG_FB)
static int syna_tcm_resume(struct device *dev)
{
	int retval;
	struct syna_tcm_hcd *tcm_hcd = dev_get_drvdata(dev);

	if (tcm_hcd->lp_state == PWR_ON)
		return 0;

	pinctrl_configure(tcm_hcd, true);

	tcm_hcd->lp_state = PWR_ON;

	tcm_hcd->prox_power_off = 0;

	if (tcm_hcd->in_hdl_mode) {
		if (!tcm_hcd->wakeup_gesture_enabled) {
			tcm_hcd->enable_irq(tcm_hcd, true, NULL);
			retval = syna_tcm_wait_hdl(tcm_hcd);
			if (retval < 0) {
				input_err(true, tcm_hcd->pdev->dev.parent,
					"Failed to wait for completion of host download\n");
				goto exit;
			}
			goto mod_resume;
		}
	} else {
		if (!tcm_hcd->wakeup_gesture_enabled)
			tcm_hcd->enable_irq(tcm_hcd, true, NULL);

#ifdef RESET_ON_RESUME
		msleep(RESET_ON_RESUME_DELAY_MS);
		goto do_reset;
#endif
	}

	if (IS_NOT_FW_MODE(tcm_hcd->id_info.mode) || tcm_hcd->app_status != APP_STATUS_OK) {
		input_info(true, tcm_hcd->pdev->dev.parent,
			"Identifying mode = 0x%02x\n", tcm_hcd->id_info.mode);
		goto do_reset;
	}

#ifdef USE_FLASH
	retval = tcm_hcd->sleep(tcm_hcd, false);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to exit deep sleep\n");
		goto exit;
	}

	retval = syna_tcm_rezero(tcm_hcd);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to rezero\n");
		goto exit;
	}

	goto mod_resume;
#endif

do_reset:
	retval = tcm_hcd->reset_n_reinit(tcm_hcd, false, true);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to do reset and reinit\n");
		goto exit;
	}

	if (IS_NOT_FW_MODE(tcm_hcd->id_info.mode) || tcm_hcd->app_status != APP_STATUS_OK) {
		input_info(true, tcm_hcd->pdev->dev.parent, "Identifying mode = 0x%02x\n",
				tcm_hcd->id_info.mode);
		retval = 0;
		goto exit;
	}

mod_resume:
	touch_resume(tcm_hcd);

#ifdef WATCHDOG_SW
	tcm_hcd->update_watchdog(tcm_hcd, true);
#endif
	cancel_delayed_work(&tcm_hcd->work_print_info);
	tcm_hcd->print_info_cnt_open = 0;
	tcm_hcd->print_info_cnt_release = 0;
	if (!shutdown_is_on_going_tsp)
		schedule_work(&tcm_hcd->work_print_info.work);

	retval = 0;

	input_info(true, tcm_hcd->pdev->dev.parent, "%s done\n", __func__);

exit:
	return retval;
}

static int syna_tcm_suspend(struct device *dev)
{
	struct syna_tcm_hcd *tcm_hcd = dev_get_drvdata(dev);

	if (tcm_hcd->lp_state == PWR_OFF)
		return 0;

	input_info(true, tcm_hcd->pdev->dev.parent, "%s start\n", __func__);

	touch_suspend(tcm_hcd);

	if (tcm_hcd->ear_detect_enable) {
		tcm_hcd->lp_state = LP_MODE;
		tcm_hcd->enable_irq(tcm_hcd, true, NULL);
	} else {
		tcm_hcd->lp_state = PWR_OFF;
		pinctrl_configure(tcm_hcd, false);
	}

	input_info(true, tcm_hcd->pdev->dev.parent, "%s done\n", __func__);

	return 0;
}

static int syna_tcm_pm_suspend(struct device *dev)
{
	struct syna_tcm_hcd *tcm_hcd = dev_get_drvdata(dev);

	reinit_completion(&tcm_hcd->resume_done);

	return 0;
}

static int syna_tcm_pm_resume(struct device *dev)
{
	struct syna_tcm_hcd *tcm_hcd = dev_get_drvdata(dev);

	complete_all(&tcm_hcd->resume_done);

	return 0;
}

#endif

#ifdef CONFIG_FB
static int syna_tcm_early_suspend(struct device *dev)
{
#ifdef USE_FLASH
	int retval;
#endif
	struct syna_tcm_hcd *tcm_hcd = dev_get_drvdata(dev);

	if (tcm_hcd->lp_state == PWR_OFF)
		return 0;

	input_info(true, tcm_hcd->pdev->dev.parent, "%s start\n", __func__);

#ifdef WATCHDOG_SW
	tcm_hcd->update_watchdog(tcm_hcd, false);
#endif
	if (!tcm_hcd->wakeup_gesture_enabled)
		tcm_hcd->enable_irq(tcm_hcd, false, false);

	if (IS_NOT_FW_MODE(tcm_hcd->id_info.mode) || tcm_hcd->app_status != APP_STATUS_OK) {
		input_info(true, tcm_hcd->pdev->dev.parent,
				"Identifying mode = 0x%02x\n", tcm_hcd->id_info.mode);
		return 0;
	}

#ifdef USE_FLASH
	if (!tcm_hcd->wakeup_gesture_enabled) {
		retval = tcm_hcd->sleep(tcm_hcd, true);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to enter deep sleep\n");
			return retval;
		}
	}
#endif

	cancel_delayed_work(&tcm_hcd->work_print_info);
	sec_ts_print_info(tcm_hcd);

	touch_early_suspend(tcm_hcd);

	input_info(true, tcm_hcd->pdev->dev.parent, "%s done\n", __func__);

	return 0;
}

static int syna_tcm_fb_notifier_cb(struct notifier_block *nb,
		unsigned long action, void *data)
{
	int retval;
	int *transition;
	struct fb_event *evdata = data;
	struct syna_tcm_hcd *tcm_hcd = container_of(nb, struct syna_tcm_hcd, fb_notifier);

	retval = 0;
	if (evdata && evdata->data && tcm_hcd) {
		transition = evdata->data;

		if (atomic_read(&tcm_hcd->firmware_flashing) && *transition == FB_BLANK_POWERDOWN) {

			retval = wait_event_interruptible_timeout(tcm_hcd->reflash_wq,
						!atomic_read(&tcm_hcd->firmware_flashing),
						msecs_to_jiffies(RESPONSE_TIMEOUT_MS));
			if (retval == 0) {
				input_err(true, tcm_hcd->pdev->dev.parent,
					"Timed out waiting for completion of flashing firmware\n");
				atomic_set(&tcm_hcd->firmware_flashing, 0);
				return -EIO;
			} else {
				retval = 0;
			}
		}

		if (action == FB_EARLY_EVENT_BLANK && *transition == FB_BLANK_POWERDOWN)
			retval = syna_tcm_early_suspend(&tcm_hcd->pdev->dev);
		else if (action == FB_EVENT_BLANK) {
			if (*transition == FB_BLANK_POWERDOWN) {
				retval = syna_tcm_suspend(&tcm_hcd->pdev->dev);
				tcm_hcd->fb_ready = 0;
			} else if (*transition == FB_BLANK_UNBLANK) {
#ifndef RESUME_EARLY_UNBLANK
				retval = syna_tcm_resume(&tcm_hcd->pdev->dev);
				tcm_hcd->fb_ready++;
#endif
			}
		} else if (action == FB_EARLY_EVENT_BLANK &&
				*transition == FB_BLANK_UNBLANK) {
#ifdef RESUME_EARLY_UNBLANK
				retval = syna_tcm_resume(&tcm_hcd->pdev->dev);
				tcm_hcd->fb_ready++;
#endif
		}
	}

	return 0;
}
#endif

static int syna_tcm_check_f35(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char fn_number;
	int retry = 0;
	const int retry_max = 10;

f35_boot_recheck:
			retval = syna_tcm_rmi_read(tcm_hcd, PDT_END_ADDR, &fn_number, sizeof(fn_number));
			if (retval < 0) {
				input_err(true, tcm_hcd->pdev->dev.parent,
					"Failed to read F35 function number\n");
				tcm_hcd->is_detected = false;
				return -ENODEV;
			}

			input_dbg(true, tcm_hcd->pdev->dev.parent, "Found F$%02x\n", fn_number);

			if (fn_number != RMI_UBL_FN_NUMBER) {
					input_err(true, tcm_hcd->pdev->dev.parent,
						"Failed to find F$35, try_times = %d\n", retry);
				if (retry < retry_max) {
					msleep(100);
					retry++;
			goto f35_boot_recheck;
				}
				tcm_hcd->is_detected = false;
				return -ENODEV;
			}
	return 0;
}

static int syna_tcm_sensor_detection(struct syna_tcm_hcd *tcm_hcd)
{
	int retval;
	unsigned char *build_id;
	unsigned int payload_length;
	unsigned int max_write_size;

	tcm_hcd->in_hdl_mode = false;
	tcm_hcd->sensor_type = TYPE_UNKNOWN;

	/* read sensor info for identification */
	retval = tcm_hcd->read_message(tcm_hcd, NULL, 0);

	/* once the tcm communication interface is not ready, */
	/* check whether the device is in F35 mode        */
	if (retval < 0) {
		if (retval == -ENXIO && tcm_hcd->hw_if->bus_io->type == BUS_SPI) {

			retval = syna_tcm_check_f35(tcm_hcd);
			if (retval < 0) {
				input_err(true, tcm_hcd->pdev->dev.parent, "Failed to read TCM message\n");
				return retval;
			}
			tcm_hcd->in_hdl_mode = true;
			tcm_hcd->sensor_type = TYPE_F35;
			tcm_hcd->is_detected = true;
			tcm_hcd->rd_chunk_size = HDL_RD_CHUNK_SIZE;
			tcm_hcd->wr_chunk_size = HDL_WR_CHUNK_SIZE;
			input_info(true, tcm_hcd->pdev->dev.parent, "F35 mode\n");

			return retval;
		} else {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to read TCM message\n");

			return retval;
		}
	}

	/* expect to get an identify report after powering on */

	if (tcm_hcd->status_report_code != REPORT_IDENTIFY) {
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Unexpected report code (0x%02x)\n", tcm_hcd->status_report_code);

		return -ENODEV;
	}

	tcm_hcd->is_detected = true;
	payload_length = tcm_hcd->payload_length;

	LOCK_BUFFER(tcm_hcd->in);

	retval = secure_memcpy((unsigned char *)&tcm_hcd->id_info,
				sizeof(tcm_hcd->id_info), &tcm_hcd->in.buf[MESSAGE_HEADER_SIZE],
				tcm_hcd->in.buf_size - MESSAGE_HEADER_SIZE,
				MIN(sizeof(tcm_hcd->id_info), payload_length));
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to copy identification info\n");
		UNLOCK_BUFFER(tcm_hcd->in);
		return retval;
	}

	UNLOCK_BUFFER(tcm_hcd->in);

	build_id = tcm_hcd->id_info.build_id;
	tcm_hcd->packrat_number = le4_to_uint(build_id);

	max_write_size = le2_to_uint(tcm_hcd->id_info.max_write_size);
	tcm_hcd->wr_chunk_size = MIN(max_write_size, WR_CHUNK_SIZE);
	if (tcm_hcd->wr_chunk_size == 0)
		tcm_hcd->wr_chunk_size = max_write_size;

	if (tcm_hcd->id_info.mode == MODE_ROMBOOTLOADER) {
		tcm_hcd->in_hdl_mode = true;
		tcm_hcd->sensor_type = TYPE_ROMBOOT;
		tcm_hcd->rd_chunk_size = HDL_RD_CHUNK_SIZE;
		tcm_hcd->wr_chunk_size = HDL_WR_CHUNK_SIZE;
		input_info(true, tcm_hcd->pdev->dev.parent, "RomBoot mode\n");
	} else if (tcm_hcd->id_info.mode == MODE_APPLICATION_FIRMWARE) {
		tcm_hcd->sensor_type = TYPE_FLASH;
		input_info(true, tcm_hcd->pdev->dev.parent,
			"Application mode (build id = %d)\n", tcm_hcd->packrat_number);
	} else {
		input_info(true, tcm_hcd->pdev->dev.parent,
			"TCM is detected, but mode is 0x%02x\n", tcm_hcd->id_info.mode);
	}

	return 0;
}

static int syna_tcm_probe(struct platform_device *pdev)
{
	int retval;
	struct syna_tcm_hcd *tcm_hcd;
	const struct syna_tcm_board_data *bdata;
	const struct syna_tcm_hw_interface *hw_if;

	hw_if = pdev->dev.platform_data;
	if (!hw_if) {
		input_err(true, &pdev->dev, "Hardware interface not found\n");
		return -ENODEV;
	}

	bdata = hw_if->bdata;
	if (!bdata) {
		input_err(true, &pdev->dev, "Board data not found\n");
		return -ENODEV;
	}

	tcm_hcd = kzalloc(sizeof(*tcm_hcd), GFP_KERNEL);
	if (!tcm_hcd) {
		input_err(true, &pdev->dev, "Failed to allocate memory for tcm_hcd\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, tcm_hcd);

	tcm_hcd->pinctrl = bdata->pinctrl;
	tcm_hcd->pdev = pdev;
	tcm_hcd->hw_if = hw_if;
	tcm_hcd->reset = syna_tcm_reset;
	tcm_hcd->reset_n_reinit = syna_tcm_reset_and_reinit;
	tcm_hcd->sleep = syna_tcm_sleep;
	tcm_hcd->identify = syna_tcm_identify;
	tcm_hcd->enable_irq = syna_tcm_enable_irq;
	tcm_hcd->switch_mode = syna_tcm_switch_mode;
	tcm_hcd->read_message = syna_tcm_read_message;
	tcm_hcd->write_message = syna_tcm_write_message;
	tcm_hcd->get_dynamic_config = syna_tcm_get_dynamic_config;
	tcm_hcd->set_dynamic_config = syna_tcm_set_dynamic_config;
	tcm_hcd->get_data_location = syna_tcm_get_data_location;

	tcm_hcd->rd_chunk_size = RD_CHUNK_SIZE;
	tcm_hcd->wr_chunk_size = WR_CHUNK_SIZE;
	tcm_hcd->is_detected = false;
	tcm_hcd->wakeup_gesture_enabled = WAKEUP_GESTURE;

#ifdef PREDICTIVE_READING
	tcm_hcd->read_length = MIN_READ_LENGTH;
#else
	tcm_hcd->read_length = MESSAGE_HEADER_SIZE;
#endif

#ifdef WATCHDOG_SW
	tcm_hcd->watchdog.run = RUN_WATCHDOG;
	tcm_hcd->update_watchdog = syna_tcm_update_watchdog;
#endif

	if (bdata->irq_gpio >= 0)
		tcm_hcd->irq = gpio_to_irq(bdata->irq_gpio);
	else
		tcm_hcd->irq = bdata->irq_gpio;

	mutex_init(&tcm_hcd->extif_mutex);
	mutex_init(&tcm_hcd->reset_mutex);
	mutex_init(&tcm_hcd->irq_en_mutex);
	mutex_init(&tcm_hcd->io_ctrl_mutex);
	mutex_init(&tcm_hcd->rw_ctrl_mutex);
	mutex_init(&tcm_hcd->command_mutex);
	mutex_init(&tcm_hcd->identify_mutex);

	INIT_BUFFER(tcm_hcd->in, false);
	INIT_BUFFER(tcm_hcd->out, false);
	INIT_BUFFER(tcm_hcd->resp, true);
	INIT_BUFFER(tcm_hcd->temp, false);
	INIT_BUFFER(tcm_hcd->config, false);
	INIT_BUFFER(tcm_hcd->report.buffer, true);

	LOCK_BUFFER(tcm_hcd->in);

	retval = syna_tcm_alloc_mem(tcm_hcd, &tcm_hcd->in, tcm_hcd->read_length + 1);
	if (retval < 0) {
		input_err(true, &pdev->dev, "Failed to allocate memory for tcm_hcd->in.buf\n");
		UNLOCK_BUFFER(tcm_hcd->in);
		goto err_alloc_mem;
	}

	UNLOCK_BUFFER(tcm_hcd->in);

	atomic_set(&tcm_hcd->command_status, CMD_IDLE);

	atomic_set(&tcm_hcd->helper.task, HELP_NONE);

	device_init_wakeup(&pdev->dev, 1);

	init_waitqueue_head(&tcm_hcd->hdl_wq);

	init_waitqueue_head(&tcm_hcd->reflash_wq);
	atomic_set(&tcm_hcd->firmware_flashing, 0);

	if (!mod_pool.initialized) {
		mutex_init(&mod_pool.mutex);
		INIT_LIST_HEAD(&mod_pool.list);
		mod_pool.initialized = true;
	}

	retval = syna_tcm_get_regulator(tcm_hcd, true);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to get regulators\n");
		goto err_get_regulator;
	}

	retval = syna_tcm_enable_regulator(tcm_hcd, true);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to enable regulators\n");
		goto err_enable_regulator;
	}

	retval = syna_tcm_config_gpio(tcm_hcd);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to configure GPIO's\n");
		goto err_config_gpio;
	}

	pinctrl_configure(tcm_hcd, true);

	/* detect the type of touch controller */
	retval = syna_tcm_sensor_detection(tcm_hcd);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to detect the sensor\n");
		goto err_config_gpio;
	}

#ifdef CONFIG_FB
	tcm_hcd->fb_notifier.notifier_call = syna_tcm_fb_notifier_cb;
	retval = fb_register_client(&tcm_hcd->fb_notifier);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to register FB notifier client\n");
	}
#endif

#ifdef REPORT_NOTIFIER
	tcm_hcd->notifier_thread = kthread_run(syna_tcm_report_notifier, tcm_hcd,
										"syna_tcm_report_notifier");
	if (IS_ERR(tcm_hcd->notifier_thread)) {
		retval = PTR_ERR(tcm_hcd->notifier_thread);
		input_err(true, tcm_hcd->pdev->dev.parent,
			"Failed to create and run tcm_hcd->notifier_thread\n");
		goto err_create_run_kthread;
	}
#endif

	tcm_hcd->helper.workqueue = create_singlethread_workqueue("syna_tcm_helper");
	INIT_WORK(&tcm_hcd->helper.work, syna_tcm_helper_work);

#ifdef WATCHDOG_SW
	tcm_hcd->watchdog.workqueue = create_singlethread_workqueue("syna_tcm_watchdog");
	INIT_DELAYED_WORK(&tcm_hcd->watchdog.work, syna_tcm_watchdog_work);
#endif

	tcm_hcd->polling_workqueue = create_singlethread_workqueue("syna_tcm_polling");
	INIT_DELAYED_WORK(&tcm_hcd->polling_work, syna_tcm_polling_work);

	INIT_DELAYED_WORK(&tcm_hcd->work_print_info, touch_print_info_work);

	/* skip the following initialization */
	/* since the fw is not ready for hdl devices */
	if (tcm_hcd->in_hdl_mode) {
		retval = zeroflash_init(tcm_hcd);
		if (retval < 0) {
			input_err(true, tcm_hcd->pdev->dev.parent, "Failed to zeroflash init\n");
			goto err_zeroflash_init;
		}
		/* goto prepare_modules; */
	}

	/* register and enable the interrupt in probe */
	/* if this is not the hdl device */
	retval = tcm_hcd->enable_irq(tcm_hcd, true, NULL);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to enable interrupt\n");
		goto err_enable_irq;
	}
	input_dbg(true, tcm_hcd->pdev->dev.parent, "Interrupt is registered\n");

	/* ensure the app firmware is running */
	retval = syna_tcm_identify(tcm_hcd, false);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Application firmware is not running\n");
		goto err_tcm_identify;
	}

	atomic_set(&tcm_hcd->host_downloading, 0);

	/* initialize the touch reporting */
	retval = touch_init(tcm_hcd);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to initialze touch reporting\n");
		goto err_tcm_identify;
	}

	retval = sec_fn_init(tcm_hcd);
	if (retval < 0) {
		input_err(true, tcm_hcd->pdev->dev.parent, "Failed to sec_fn_init\n");
		goto err_touch_init;
	}

/*prepare_modules:*/
	/* prepare to add other modules */
	mod_pool.workqueue = create_singlethread_workqueue("syna_tcm_module");
	INIT_WORK(&mod_pool.work, syna_tcm_module_work);
	mod_pool.tcm_hcd = tcm_hcd;
	mod_pool.queue_work = true;
	queue_work(mod_pool.workqueue, &mod_pool.work);

	INIT_DELAYED_WORK(&tcm_hcd->work_read_info, sec_read_info_work);
	if (!shutdown_is_on_going_tsp)
		schedule_delayed_work(&tcm_hcd->work_read_info, msecs_to_jiffies(50));

	init_completion(&tcm_hcd->resume_done);
	complete_all(&tcm_hcd->resume_done);

	return 0;

err_touch_init:
	touch_remove(tcm_hcd);
err_tcm_identify:
	tcm_hcd->enable_irq(tcm_hcd, false, NULL);
err_enable_irq:
	zeroflash_remove(tcm_hcd);
err_zeroflash_init:
	cancel_delayed_work_sync(&tcm_hcd->polling_work);
	flush_workqueue(tcm_hcd->polling_workqueue);
	destroy_workqueue(tcm_hcd->polling_workqueue);

#ifdef WATCHDOG_SW
	cancel_delayed_work_sync(&tcm_hcd->watchdog.work);
	flush_workqueue(tcm_hcd->watchdog.workqueue);
	destroy_workqueue(tcm_hcd->watchdog.workqueue);
#endif

	cancel_work_sync(&tcm_hcd->helper.work);
	flush_workqueue(tcm_hcd->helper.workqueue);
	destroy_workqueue(tcm_hcd->helper.workqueue);

#ifdef REPORT_NOTIFIER
	kthread_stop(tcm_hcd->notifier_thread);

err_create_run_kthread:
#endif
#ifdef CONFIG_FB
	fb_unregister_client(&tcm_hcd->fb_notifier);
#endif

	if (bdata->irq_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->irq_gpio, false, 0, 0);

	if (bdata->cs_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->cs_gpio, false, 0, 0);

	if (bdata->power_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->power_gpio, false, 0, 0);

	if (bdata->reset_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->reset_gpio, false, 0, 0);

err_config_gpio:
	syna_tcm_enable_regulator(tcm_hcd, false);

err_enable_regulator:
	syna_tcm_get_regulator(tcm_hcd, false);

err_get_regulator:
	device_init_wakeup(&pdev->dev, 0);

err_alloc_mem:
	RELEASE_BUFFER(tcm_hcd->report.buffer);
	RELEASE_BUFFER(tcm_hcd->config);
	RELEASE_BUFFER(tcm_hcd->temp);
	RELEASE_BUFFER(tcm_hcd->resp);
	RELEASE_BUFFER(tcm_hcd->out);
	RELEASE_BUFFER(tcm_hcd->in);

	kfree(tcm_hcd);

	return retval;
}

static int syna_tcm_remove(struct platform_device *pdev)
{
	struct syna_tcm_module_handler *mod_handler;
	struct syna_tcm_module_handler *tmp_handler;
	struct syna_tcm_hcd *tcm_hcd = platform_get_drvdata(pdev);
	const struct syna_tcm_board_data *bdata = tcm_hcd->hw_if->bdata;

	input_info(true, pdev->dev.parent, "%s\n", __func__);
	shutdown_is_on_going_tsp = true;

	if (tcm_hcd->irq_enabled && bdata->irq_gpio >= 0) {
		disable_irq(tcm_hcd->irq);
		free_irq(tcm_hcd->irq, tcm_hcd);
	}

	touch_remove(tcm_hcd);

	mutex_lock(&mod_pool.mutex);

	if (!list_empty(&mod_pool.list)) {
		list_for_each_entry_safe(mod_handler, tmp_handler, &mod_pool.list, link) {
			if (mod_handler->mod_cb->remove)
				mod_handler->mod_cb->remove(tcm_hcd);
			list_del(&mod_handler->link);
			kfree(mod_handler);
		}
	}

	mod_pool.queue_work = false;
	cancel_work_sync(&mod_pool.work);
	flush_workqueue(mod_pool.workqueue);
	destroy_workqueue(mod_pool.workqueue);

	mutex_unlock(&mod_pool.mutex);

	cancel_delayed_work_sync(&tcm_hcd->polling_work);
	flush_workqueue(tcm_hcd->polling_workqueue);
	destroy_workqueue(tcm_hcd->polling_workqueue);

	cancel_delayed_work_sync(&tcm_hcd->work_print_info);
	cancel_delayed_work_sync(&tcm_hcd->work_read_info);

#ifdef WATCHDOG_SW
	cancel_delayed_work_sync(&tcm_hcd->watchdog.work);
	flush_workqueue(tcm_hcd->watchdog.workqueue);
	destroy_workqueue(tcm_hcd->watchdog.workqueue);
#endif

	cancel_work_sync(&tcm_hcd->helper.work);
	flush_workqueue(tcm_hcd->helper.workqueue);
	destroy_workqueue(tcm_hcd->helper.workqueue);

#ifdef REPORT_NOTIFIER
	kthread_stop(tcm_hcd->notifier_thread);
#endif

#ifdef CONFIG_FB
	fb_unregister_client(&tcm_hcd->fb_notifier);
#endif

	if (bdata->irq_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->irq_gpio, false, 0, 0);

	if (bdata->cs_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->cs_gpio, false, 0, 0);

	if (bdata->power_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->power_gpio, false, 0, 0);

	if (bdata->reset_gpio >= 0)
		syna_tcm_set_gpio(tcm_hcd, bdata->reset_gpio, false, 0, 0);

	syna_tcm_enable_regulator(tcm_hcd, false);

	syna_tcm_get_regulator(tcm_hcd, false);

	device_init_wakeup(&pdev->dev, 0);

	RELEASE_BUFFER(tcm_hcd->report.buffer);
	RELEASE_BUFFER(tcm_hcd->config);
	RELEASE_BUFFER(tcm_hcd->temp);
	RELEASE_BUFFER(tcm_hcd->resp);
	RELEASE_BUFFER(tcm_hcd->out);
	RELEASE_BUFFER(tcm_hcd->in);

	sec_fn_remove(tcm_hcd);

	kfree(tcm_hcd);

	return 0;
}

static void syna_tcm_shutdown(struct platform_device *pdev)
{
	int retval;

	retval = syna_tcm_remove(pdev);
}

#ifdef CONFIG_PM
static const struct dev_pm_ops syna_tcm_dev_pm_ops = {
/*#ifndef CONFIG_FB*/
	.suspend = syna_tcm_pm_suspend,
	.resume = syna_tcm_pm_resume,
/*#endif*/
};
#endif

static struct platform_driver syna_tcm_driver = {
	.driver = {
		.name = PLATFORM_DRIVER_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &syna_tcm_dev_pm_ops,
#endif
	},
	.probe = syna_tcm_probe,
	.remove = syna_tcm_remove,
	.shutdown = syna_tcm_shutdown,
};

static int __init syna_tcm_module_init(void)
{
	int retval;

	retval = syna_tcm_bus_init();
	if (retval < 0)
		return retval;

	return platform_driver_register(&syna_tcm_driver);
}

static void __exit syna_tcm_module_exit(void)
{
	platform_driver_unregister(&syna_tcm_driver);

	syna_tcm_bus_exit();

	return;
}

module_init(syna_tcm_module_init);
module_exit(syna_tcm_module_exit);

MODULE_AUTHOR("Synaptics, Inc.");
MODULE_DESCRIPTION("Synaptics TCM Touch Driver");
MODULE_LICENSE("GPL v2");
