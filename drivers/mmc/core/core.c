/*
 *  linux/drivers/mmc/core/core.c
 *
 *  Copyright (C) 2003-2004 Russell King, All Rights Reserved.
 *  SD support Copyright (C) 2004 Ian Molton, All Rights Reserved.
 *  Copyright (C) 2005-2008 Pierre Ossman, All Rights Reserved.
 *  MMCv4 support Copyright (C) 2006 Philip Langdale, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/pagemap.h>
#include <linux/err.h>
#include <linux/leds.h>
#include <linux/scatterlist.h>
#include <linux/log2.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/fault-inject.h>
#include <linux/random.h>
#include <linux/wakelock.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/mmc_trace.h>

#include "core.h"
#include "bus.h"
#include "host.h"
#include "sdio_bus.h"

#include "mmc_ops.h"
#include "sd_ops.h"
#include "sdio_ops.h"

#if defined(CONFIG_BLK_DEV_IO_TRACE)
#include "../card/queue.h"
#endif

static struct workqueue_struct *workqueue;

/*
 * Enabling software CRCs on the data blocks can be a significant (30%)
 * performance cost, and for other reasons may not always be desired.
 * So we allow it it to be disabled.
 */
bool use_spi_crc = 0;
module_param(use_spi_crc, bool, 0);

/*
 * We normally treat cards as removed during suspend if they are not
 * known to be on a non-removable bus, to avoid the risk of writing
 * back data to a different card after resume.  Allow this to be
 * overridden if necessary.
 */
#ifdef CONFIG_MMC_UNSAFE_RESUME
bool mmc_assume_removable;
#else
bool mmc_assume_removable = 1;
#endif
EXPORT_SYMBOL(mmc_assume_removable);
module_param_named(removable, mmc_assume_removable, bool, 0644);
MODULE_PARM_DESC(
	removable,
	"MMC/SD cards are removable and may be removed during suspend");

/*
 * Internal function. Schedule delayed work in the MMC work queue.
 */
static int mmc_schedule_delayed_work(struct delayed_work *work,
				     unsigned long delay)
{
	return queue_delayed_work(workqueue, work, delay);
}

/*
 * Internal function. Flush all scheduled work from the MMC work queue.
 */
static void mmc_flush_scheduled_work(void)
{
	flush_workqueue(workqueue);
}

#ifdef CONFIG_FAIL_MMC_REQUEST

/*
 * Internal function. Inject random data errors.
 * If mmc_data is NULL no errors are injected.
 */
static void mmc_should_fail_request(struct mmc_host *host,
				    struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;
	struct mmc_data *data = mrq->data;
	static const int data_errors[] = {
		-ETIMEDOUT,
		-EILSEQ,
		-EIO,
	};

	if (!data)
		return;

	if (cmd->error || data->error ||
	    !should_fail(&host->fail_mmc_request, data->blksz * data->blocks))
		return;

	data->error = data_errors[random32() % ARRAY_SIZE(data_errors)];
	data->bytes_xfered = (random32() % (data->bytes_xfered >> 9)) << 9;
}

#else /* CONFIG_FAIL_MMC_REQUEST */

static inline void mmc_should_fail_request(struct mmc_host *host,
					   struct mmc_request *mrq)
{
}

#endif /* CONFIG_FAIL_MMC_REQUEST */

/**
 *	mmc_request_done - finish processing an MMC request
 *	@host: MMC host which completed request
 *	@mrq: MMC request which request
 *
 *	MMC drivers should call this function when they have completed
 *	their processing of a request.
 */
void mmc_request_done(struct mmc_host *host, struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;
	int err = cmd->error;

	if (err && cmd->retries && mmc_host_is_spi(host)) {
		if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
			cmd->retries = 0;
	}

	if (err && cmd->retries && !mmc_card_removed(host->card)) {
		/*
		 * Request starter must handle retries - see
		 * mmc_wait_for_req_done().
		 */
		if (mrq->done)
			mrq->done(mrq);
	} else {
		mmc_should_fail_request(host, mrq);

		led_trigger_event(host->led, LED_OFF);

		pr_debug("%s: req done (CMD%u): %d: %08x %08x %08x %08x\n",
			mmc_hostname(host), cmd->opcode, err,
			cmd->resp[0], cmd->resp[1],
			cmd->resp[2], cmd->resp[3]);

		if (mrq->data) {
			pr_debug("%s:     %d bytes transferred: %d\n",
				mmc_hostname(host),
				mrq->data->bytes_xfered, mrq->data->error);
		}

		if (mrq->stop) {
			pr_debug("%s:     (CMD%u): %d: %08x %08x %08x %08x\n",
				mmc_hostname(host), mrq->stop->opcode,
				mrq->stop->error,
				mrq->stop->resp[0], mrq->stop->resp[1],
				mrq->stop->resp[2], mrq->stop->resp[3]);
		}

		if (mrq->done)
			mrq->done(mrq);

		mmc_host_clk_release(host);
	}
}

EXPORT_SYMBOL(mmc_request_done);

static void
mmc_start_request(struct mmc_host *host, struct mmc_request *mrq)
{
#ifdef CONFIG_MMC_DEBUG
	unsigned int i, sz;
	struct scatterlist *sg;
#endif

	if (mrq->sbc) {
		pr_debug("<%s: starting CMD%u arg %08x flags %08x>\n",
			 mmc_hostname(host), mrq->sbc->opcode,
			 mrq->sbc->arg, mrq->sbc->flags);
	}

	pr_debug("%s: starting CMD%u arg %08x flags %08x\n",
		 mmc_hostname(host), mrq->cmd->opcode,
		 mrq->cmd->arg, mrq->cmd->flags);

	if (mrq->data) {
		pr_debug("%s:     blksz %d blocks %d flags %08x "
			"tsac %d ms nsac %d\n",
			mmc_hostname(host), mrq->data->blksz,
			mrq->data->blocks, mrq->data->flags,
			mrq->data->timeout_ns / 1000000,
			mrq->data->timeout_clks);
	}

	if (mrq->stop) {
		pr_debug("%s:     CMD%u arg %08x flags %08x\n",
			 mmc_hostname(host), mrq->stop->opcode,
			 mrq->stop->arg, mrq->stop->flags);
	}

	WARN_ON(!host->claimed);

	mrq->cmd->error = 0;
	mrq->cmd->mrq = mrq;
	if (mrq->data) {
		BUG_ON(mrq->data->blksz > host->max_blk_size);
		BUG_ON(mrq->data->blocks > host->max_blk_count);
		BUG_ON(mrq->data->blocks * mrq->data->blksz >
			host->max_req_size);

#ifdef CONFIG_MMC_DEBUG
		sz = 0;
		for_each_sg(mrq->data->sg, sg, mrq->data->sg_len, i)
			sz += sg->length;
		BUG_ON(sz != mrq->data->blocks * mrq->data->blksz);
#endif

		mrq->cmd->data = mrq->data;
		mrq->data->error = 0;
		mrq->data->mrq = mrq;
		if (mrq->stop) {
			mrq->data->stop = mrq->stop;
			mrq->stop->error = 0;
			mrq->stop->mrq = mrq;
		}
	}
	mmc_host_clk_hold(host);
	led_trigger_event(host->led, LED_FULL);
	host->ops->request(host, mrq);
}

/*
 * mmc_wait_data_done() - done callback for data request
 * @mrq: done data request
 *
 * Wakes up mmc context, passed as a callback to host controller driver
 */
static void mmc_wait_data_done(struct mmc_request *mrq)
{
	unsigned long flags;

	spin_lock_irqsave(&mrq->host->context_info.lock, flags);
	mrq->host->context_info.is_done_rcv = true;
	wake_up_interruptible(&mrq->host->context_info.wait);
	spin_unlock_irqrestore(&mrq->host->context_info.lock, flags);
}

static void mmc_wait_done(struct mmc_request *mrq)
{
	complete(&mrq->completion);
}

/*
 *__mmc_start_data_req() - starts data request
 * @host: MMC host to start the request
 * @mrq: data request to start
 *
 * Sets the done callback to be called when request is completed by the card.
 * Starts data mmc request execution
 */
static int __mmc_start_data_req(struct mmc_host *host, struct mmc_request *mrq)
{
	mrq->done = mmc_wait_data_done;
	mrq->host = host;
	if (mmc_card_removed(host->card)) {
		mrq->cmd->error = -ENOMEDIUM;
		mmc_wait_data_done(mrq);
		return -ENOMEDIUM;
	}
	mmc_start_request(host, mrq);

	return 0;
}

static int __mmc_start_req(struct mmc_host *host, struct mmc_request *mrq)
{
	init_completion(&mrq->completion);
	mrq->done = mmc_wait_done;
	if (mmc_card_removed(host->card)) {
		mrq->cmd->error = -ENOMEDIUM;
		complete(&mrq->completion);
		return -ENOMEDIUM;
	}
	mmc_start_request(host, mrq);
	return 0;
}

/*
 * mmc_wait_for_data_req_done() - wait for request completed
 * @host: MMC host to prepare the command.
 * @mrq: MMC request to wait for
 *
 * Blocks MMC context till host controller will ack end of data request
 * execution or new request notification arrives from the block layer.
 * Handles command retries.
 *
 * Returns enum mmc_blk_status after checking errors.
 */
static int mmc_wait_for_data_req_done(struct mmc_host *host,
				      struct mmc_request *mrq,
				      struct mmc_async_req *next_req)
{
	struct mmc_command *cmd;
	struct mmc_context_info *context_info = &host->context_info;
	int err;
	unsigned long flags;

	while (1) {
		wait_event_interruptible(context_info->wait,
				(context_info->is_done_rcv ||
				 context_info->is_new_req));
		spin_lock_irqsave(&context_info->lock, flags);
		context_info->is_waiting_last_req = false;
		if (context_info->is_done_rcv) {
			context_info->is_done_rcv = false;
			context_info->is_new_req = false;
			spin_unlock_irqrestore(&context_info->lock, flags);
			cmd = mrq->cmd;
#if defined(CONFIG_MACH_JA_KOR_SKT) || defined(CONFIG_MACH_JA_KOR_KT) || defined(CONFIG_MACH_JA_KOR_LGT) || defined(CONFIG_MACH_J_CHN_CU)
			if ((!cmd->error && cmd->data->error != -EILSEQ) ||
				!cmd->retries || mmc_card_removed(host->card)) {
#else
			if (!cmd->error || !cmd->retries ||
					mmc_card_removed(host->card)) {
#endif
				err = host->areq->err_check(host->card,
						host->areq);

				mmc_add_trace(__MMC_TA_REQ_DONE,
							host->mqrq_prev);
				break; /* return err */
			} else {
				pr_info("%s: req failed (CMD%u): %d, retrying...\n",
						mmc_hostname(host),
						cmd->opcode, cmd->error);
				cmd->retries--;
				cmd->error = 0;
#if defined(CONFIG_MACH_JA_KOR_SKT) || defined(CONFIG_MACH_JA_KOR_KT) || defined(CONFIG_MACH_JA_KOR_LGT) || defined(CONFIG_MACH_J_CHN_CU)
				mmc_host_clk_hold(host);
				cmd->data->error = 0;
#endif
				host->ops->request(host, mrq);
				continue; /* wait for done/new event again */
			}
		} else if (context_info->is_new_req) {
			context_info->is_new_req = false;
			if (!next_req) {
				spin_unlock_irqrestore(&context_info->lock,
							flags);
				err = MMC_BLK_NEW_REQUEST;
				break; /* return err */
			}
		}
		spin_unlock_irqrestore(&context_info->lock, flags);
	} /* while */
	return err;
}

static void mmc_wait_for_req_done(struct mmc_host *host,
				  struct mmc_request *mrq)
{
	struct mmc_command *cmd;

	while (1) {
		wait_for_completion(&mrq->completion);

		cmd = mrq->cmd;
		if (!cmd->error || !cmd->retries ||
		    mmc_card_removed(host->card))
			break;

		pr_debug("%s: req failed (CMD%u): %d, retrying...\n",
			 mmc_hostname(host), cmd->opcode, cmd->error);
		cmd->retries--;
		cmd->error = 0;
		host->ops->request(host, mrq);
	}
}

/**
 *	mmc_pre_req - Prepare for a new request
 *	@host: MMC host to prepare command
 *	@mrq: MMC request to prepare for
 *	@is_first_req: true if there is no previous started request
 *                     that may run in parellel to this call, otherwise false
 *
 *	mmc_pre_req() is called in prior to mmc_start_req() to let
 *	host prepare for the new request. Preparation of a request may be
 *	performed while another request is running on the host.
 */
static void mmc_pre_req(struct mmc_host *host, struct mmc_request *mrq,
		 bool is_first_req)
{
	if (host->ops->pre_req) {
		mmc_host_clk_hold(host);
		host->ops->pre_req(host, mrq, is_first_req);
		mmc_host_clk_release(host);
	}
}

/**
 *	mmc_post_req - Post process a completed request
 *	@host: MMC host to post process command
 *	@mrq: MMC request to post process for
 *	@err: Error, if non zero, clean up any resources made in pre_req
 *
 *	Let the host post process a completed request. Post processing of
 *	a request may be performed while another reuqest is running.
 */
static void mmc_post_req(struct mmc_host *host, struct mmc_request *mrq,
			 int err)
{
	if (host->ops->post_req) {
		mmc_host_clk_hold(host);
		host->ops->post_req(host, mrq, err);
		mmc_host_clk_release(host);
	}
}

/**
 *	mmc_start_req - start a non-blocking request
 *	@host: MMC host to start command
 *	@areq: async request to start
 *	@error: out parameter returns 0 for success, otherwise non zero
 *
 *	Start a new MMC custom command request for a host.
 *	If there is on ongoing async request wait for completion
 *	of that request and start the new one and return.
 *	Does not wait for the new request to complete.
 *
 *      Returns the completed request, NULL in case of none completed.
 *	Wait for the an ongoing request (previoulsy started) to complete and
 *	return the completed request. If there is no ongoing request, NULL
 *	is returned without waiting. NULL is not an error condition.
 */
struct mmc_async_req *mmc_start_req(struct mmc_host *host,
				    struct mmc_async_req *areq, int *error)
{
	int err = 0;
	int start_err = 0;
	struct mmc_async_req *data = host->areq;

	/* Prepare a new request */
	if (areq && !areq->__cond) {
		mmc_pre_req(host, areq->mrq, !host->areq);
		if (areq->__mrq) {
			mmc_pre_req(host, areq->__mrq, 0);
		}
		mmc_add_trace(__MMC_TA_PRE_DONE, host->mqrq_cur);
	}

	if (host->areq) {
		err = mmc_wait_for_data_req_done(host, host->areq->mrq,
				areq);
		if (err == MMC_BLK_NEW_REQUEST) {
			if (error)
				*error = err;
			/*
			 * The previous request was not completed,
			 * nothing to return
			 */
			return NULL;
		}
	}

	if (!err && areq) {
		mmc_add_trace(__MMC_TA_MMC_ISSUE, host->mqrq_cur);
		start_err = __mmc_start_data_req(host, areq->mrq);
		mmc_add_trace(__MMC_TA_MMC_DONE, host->mqrq_cur);
	}

	if (host->areq)
		mmc_post_req(host, host->areq->mrq, 0);

	 /* Cancel a prepared request if it was not started. */
	if ((err || start_err) && areq) {
		mmc_post_req(host, areq->mrq, -EINVAL);
		if (areq->__mrq)
			mmc_post_req(host, areq->__mrq, -EINVAL);
	}

	if (err)
		host->areq = NULL;
	else
		host->areq = areq;

	if (error)
		*error = err;
	return data;
}
EXPORT_SYMBOL(mmc_start_req);

/**
 *	mmc_wait_for_req - start a request and wait for completion
 *	@host: MMC host to start command
 *	@mrq: MMC request to start
 *
 *	Start a new MMC custom command request for a host, and wait
 *	for the command to complete. Does not attempt to parse the
 *	response.
 */
void mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq)
{
	__mmc_start_req(host, mrq);
	mmc_wait_for_req_done(host, mrq);
}
EXPORT_SYMBOL(mmc_wait_for_req);

/**
 *	mmc_interrupt_hpi - Issue for High priority Interrupt
 *	@card: the MMC card associated with the HPI transfer
 *
 *	Issued High Priority Interrupt, and check for card status
 *	util out-of prg-state.
 */
int mmc_interrupt_hpi(struct mmc_card *card)
{
	int err;
	u32 status;

	BUG_ON(!card);

	if (!card->ext_csd.hpi_en) {
		pr_info("%s: HPI enable bit unset\n", mmc_hostname(card->host));
		return 1;
	}

	mmc_claim_host(card->host);
	err = mmc_send_status(card, &status, 0);
	if (err) {
		pr_err("%s: Get card status fail\n", mmc_hostname(card->host));
		goto out;
	}

	/*
	 * If the card status is in PRG-state, we can send the HPI command.
	 */
	if (R1_CURRENT_STATE(status) == R1_STATE_PRG) {
		do {
			/*
			 * We don't know when the HPI command will finish
			 * processing, so we need to resend HPI until out
			 * of prg-state, and keep checking the card status
			 * with SEND_STATUS.  If a timeout error occurs when
			 * sending the HPI command, we are already out of
			 * prg-state.
			 */
			err = mmc_send_hpi_cmd(card, &status);
			if (err)
				pr_debug("%s: abort HPI (%d error)\n",
					 mmc_hostname(card->host), err);

			err = mmc_send_status(card, &status, 0);
			if (err)
				break;
		} while (R1_CURRENT_STATE(status) == R1_STATE_PRG);
	} else
		pr_debug("%s: Left prg-state\n", mmc_hostname(card->host));

out:
	mmc_release_host(card->host);
	return err;
}
EXPORT_SYMBOL(mmc_interrupt_hpi);

/**
 *	mmc_wait_for_cmd - start a command and wait for completion
 *	@host: MMC host to start command
 *	@cmd: MMC command to start
 *	@retries: maximum number of retries
 *
 *	Start a new MMC command for a host, and wait for the command
 *	to complete.  Return any error that occurred while the command
 *	was executing.  Do not attempt to parse the response.
 */
int mmc_wait_for_cmd(struct mmc_host *host, struct mmc_command *cmd, int retries)
{
	struct mmc_request mrq = {NULL};

	WARN_ON(!host->claimed);

	memset(cmd->resp, 0, sizeof(cmd->resp));
	cmd->retries = retries;

	mrq.cmd = cmd;
	cmd->data = NULL;

	mmc_wait_for_req(host, &mrq);

	return cmd->error;
}

EXPORT_SYMBOL(mmc_wait_for_cmd);

/**
 *	mmc_set_data_timeout - set the timeout for a data command
 *	@data: data phase for command
 *	@card: the MMC card associated with the data transfer
 *
 *	Computes the data timeout parameters according to the
 *	correct algorithm given the card type.
 */
void mmc_set_data_timeout(struct mmc_data *data, const struct mmc_card *card)
{
	unsigned int mult;

	/*
	 * SDIO cards only define an upper 1 s limit on access.
	 */
	if (mmc_card_sdio(card)) {
		data->timeout_ns = 1000000000;
		data->timeout_clks = 0;
		return;
	}

	/*
	 * SD cards use a 100 multiplier rather than 10
	 */
	mult = mmc_card_sd(card) ? 100 : 10;

	/*
	 * Scale up the multiplier (and therefore the timeout) by
	 * the r2w factor for writes.
	 */
	if (data->flags & MMC_DATA_WRITE)
		mult <<= card->csd.r2w_factor;

	data->timeout_ns = card->csd.tacc_ns * mult;
	data->timeout_clks = card->csd.tacc_clks * mult;

	/*
	 * SD cards also have an upper limit on the timeout.
	 */
	if (mmc_card_sd(card)) {
		unsigned int timeout_us, limit_us;

		timeout_us = data->timeout_ns / 1000;
		if (mmc_host_clk_rate(card->host))
			timeout_us += data->timeout_clks * 1000 /
				(mmc_host_clk_rate(card->host) / 1000);

		if (data->flags & MMC_DATA_WRITE)
			/*
			 * The MMC spec "It is strongly recommended
			 * for hosts to implement more than 500ms
			 * timeout value even if the card indicates
			 * the 250ms maximum busy length."  Even the
			 * previous value of 300ms is known to be
			 * insufficient for some cards.
			 */
			limit_us = 3000000;
		else
			limit_us = 100000;

		/*
		 * SDHC cards always use these fixed values.
		 */
		if (timeout_us > limit_us || mmc_card_blockaddr(card)) {
			data->timeout_ns = limit_us * 1000;
			data->timeout_clks = 0;
		}
	}

	/*
	 * Some cards require longer data read timeout than indicated in CSD.
	 * Address this by setting the read timeout to a "reasonably high"
	 * value. For the cards tested, 300ms has proven enough. If necessary,
	 * this value can be increased if other problematic cards require this.
	 */
	if (mmc_card_long_read_time(card) && data->flags & MMC_DATA_READ) {
		data->timeout_ns = 300000000;
		data->timeout_clks = 0;
	}

	/*
	 * Some cards need very high timeouts if driven in SPI mode.
	 * The worst observed timeout was 900ms after writing a
	 * continuous stream of data until the internal logic
	 * overflowed.
	 */
	if (mmc_host_is_spi(card->host)) {
		if (data->flags & MMC_DATA_WRITE) {
			if (data->timeout_ns < 1000000000)
				data->timeout_ns = 1000000000;	/* 1s */
		} else {
			if (data->timeout_ns < 100000000)
				data->timeout_ns =  100000000;	/* 100ms */
		}
	}
}
EXPORT_SYMBOL(mmc_set_data_timeout);

/**
 *	mmc_align_data_size - pads a transfer size to a more optimal value
 *	@card: the MMC card associated with the data transfer
 *	@sz: original transfer size
 *
 *	Pads the original data size with a number of extra bytes in
 *	order to avoid controller bugs and/or performance hits
 *	(e.g. some controllers revert to PIO for certain sizes).
 *
 *	Returns the improved size, which might be unmodified.
 *
 *	Note that this function is only relevant when issuing a
 *	single scatter gather entry.
 */
unsigned int mmc_align_data_size(struct mmc_card *card, unsigned int sz)
{
	unsigned int align = card->host->align_size;

	sz = ((sz + (align - 1)) / align) * align;

	return sz;
}
EXPORT_SYMBOL(mmc_align_data_size);

/**
 *	__mmc_claim_host - exclusively claim a host
 *	@host: mmc host to claim
 *	@abort: whether or not the operation should be aborted
 *
 *	Claim a host for a set of operations.  If @abort is non null and
 *	dereference a non-zero value then this will return prematurely with
 *	that non-zero value without acquiring the lock.  Returns zero
 *	with the lock held otherwise.
 */
int __mmc_claim_host(struct mmc_host *host, atomic_t *abort)
{
	DECLARE_WAITQUEUE(wait, current);
	unsigned long flags;
	int stop;

	might_sleep();

	add_wait_queue(&host->wq, &wait);
	spin_lock_irqsave(&host->lock, flags);
	while (1) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		stop = abort ? atomic_read(abort) : 0;
		if (stop || !host->claimed || host->claimer == current)
			break;
		spin_unlock_irqrestore(&host->lock, flags);
		schedule();
		spin_lock_irqsave(&host->lock, flags);
	}
	set_current_state(TASK_RUNNING);
	if (!stop) {
		host->claimed = 1;
		host->claimer = current;
		host->claim_cnt += 1;
	} else
		wake_up(&host->wq);
	spin_unlock_irqrestore(&host->lock, flags);
	remove_wait_queue(&host->wq, &wait);
	if (host->ops->enable && !stop && host->claim_cnt == 1)
		host->ops->enable(host);
	return stop;
}

EXPORT_SYMBOL(__mmc_claim_host);

/**
 *	mmc_try_claim_host - try exclusively to claim a host
 *	@host: mmc host to claim
 *
 *	Returns %1 if the host is claimed, %0 otherwise.
 */
int mmc_try_claim_host(struct mmc_host *host)
{
	int claimed_host = 0;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	if (!host->claimed || host->claimer == current) {
		host->claimed = 1;
		host->claimer = current;
		host->claim_cnt += 1;
		claimed_host = 1;
	}
	spin_unlock_irqrestore(&host->lock, flags);
	if (host->ops->enable && claimed_host && host->claim_cnt == 1)
		host->ops->enable(host);
	return claimed_host;
}
EXPORT_SYMBOL(mmc_try_claim_host);

/**
 *	mmc_release_host - release a host
 *	@host: mmc host to release
 *
 *	Release a MMC host, allowing others to claim the host
 *	for their operations.
 */
void mmc_release_host(struct mmc_host *host)
{
	unsigned long flags;

	WARN_ON(!host->claimed);

	if (host->ops->disable && host->claim_cnt == 1)
		host->ops->disable(host);

	spin_lock_irqsave(&host->lock, flags);
	if (--host->claim_cnt) {
		/* Release for nested claim */
		spin_unlock_irqrestore(&host->lock, flags);
	} else {
		host->claimed = 0;
		host->claimer = NULL;
		spin_unlock_irqrestore(&host->lock, flags);
		wake_up(&host->wq);
	}
}
EXPORT_SYMBOL(mmc_release_host);

/*
 * Internal function that does the actual ios call to the host driver,
 * optionally printing some debug output.
 */
static inline void mmc_set_ios(struct mmc_host *host)
{
	struct mmc_ios *ios = &host->ios;

	pr_debug("%s: clock %uHz busmode %u powermode %u cs %u Vdd %u "
		"width %u timing %u\n",
		 mmc_hostname(host), ios->clock, ios->bus_mode,
		 ios->power_mode, ios->chip_select, ios->vdd,
		 ios->bus_width, ios->timing);

	if (ios->clock > 0)
		mmc_set_ungated(host);
	host->ops->set_ios(host, ios);
}

/*
 * Control chip select pin on a host.
 */
void mmc_set_chip_select(struct mmc_host *host, int mode)
{
	mmc_host_clk_hold(host);
	host->ios.chip_select = mode;
	mmc_set_ios(host);
	mmc_host_clk_release(host);
}

/*
 * Sets the host clock to the highest possible frequency that
 * is below "hz".
 */
static void __mmc_set_clock(struct mmc_host *host, unsigned int hz)
{
	WARN_ON(hz < host->f_min);

	if (hz > host->f_max)
		hz = host->f_max;

	host->ios.clock = hz;
	mmc_set_ios(host);
}

void mmc_set_clock(struct mmc_host *host, unsigned int hz)
{
	mmc_host_clk_hold(host);
	__mmc_set_clock(host, hz);
	mmc_host_clk_release(host);
}

#ifdef CONFIG_MMC_CLKGATE
/*
 * This gates the clock by setting it to 0 Hz.
 */
void mmc_gate_clock(struct mmc_host *host)
{
	unsigned long flags;

	spin_lock_irqsave(&host->clk_lock, flags);
	host->clk_old = host->ios.clock;
	host->ios.clock = 0;
	host->clk_gated = true;
	spin_unlock_irqrestore(&host->clk_lock, flags);
	mmc_set_ios(host);
}

/*
 * This restores the clock from gating by using the cached
 * clock value.
 */
void mmc_ungate_clock(struct mmc_host *host)
{
	/*
	 * We should previously have gated the clock, so the clock shall
	 * be 0 here! The clock may however be 0 during initialization,
	 * when some request operations are performed before setting
	 * the frequency. When ungate is requested in that situation
	 * we just ignore the call.
	 */
	if (host->clk_old) {
		BUG_ON(host->ios.clock);
		/* This call will also set host->clk_gated to false */
		__mmc_set_clock(host, host->clk_old);
	}
}

void mmc_set_ungated(struct mmc_host *host)
{
	unsigned long flags;

	/*
	 * We've been given a new frequency while the clock is gated,
	 * so make sure we regard this as ungating it.
	 */
	spin_lock_irqsave(&host->clk_lock, flags);
	host->clk_gated = false;
	spin_unlock_irqrestore(&host->clk_lock, flags);
}

#else
void mmc_set_ungated(struct mmc_host *host)
{
}
#endif

/*
 * Change the bus mode (open drain/push-pull) of a host.
 */
void mmc_set_bus_mode(struct mmc_host *host, unsigned int mode)
{
	mmc_host_clk_hold(host);
	host->ios.bus_mode = mode;
	mmc_set_ios(host);
	mmc_host_clk_release(host);
}

/*
 * Change data bus width of a host.
 */
void mmc_set_bus_width(struct mmc_host *host, unsigned int width)
{
	mmc_host_clk_hold(host);
	host->ios.bus_width = width;
	mmc_set_ios(host);
	mmc_host_clk_release(host);
}

/**
 * mmc_vdd_to_ocrbitnum - Convert a voltage to the OCR bit number
 * @vdd:	voltage (mV)
 * @low_bits:	prefer low bits in boundary cases
 *
 * This function returns the OCR bit number according to the provided @vdd
 * value. If conversion is not possible a negative errno value returned.
 *
 * Depending on the @low_bits flag the function prefers low or high OCR bits
 * on boundary voltages. For example,
 * with @low_bits = true, 3300 mV translates to ilog2(MMC_VDD_32_33);
 * with @low_bits = false, 3300 mV translates to ilog2(MMC_VDD_33_34);
 *
 * Any value in the [1951:1999] range translates to the ilog2(MMC_VDD_20_21).
 */
static int mmc_vdd_to_ocrbitnum(int vdd, bool low_bits)
{
	const int max_bit = ilog2(MMC_VDD_35_36);
	int bit;

	if (vdd < 1650 || vdd > 3600)
		return -EINVAL;

	if (vdd >= 1650 && vdd <= 1950)
		return ilog2(MMC_VDD_165_195);

	if (low_bits)
		vdd -= 1;

	/* Base 2000 mV, step 100 mV, bit's base 8. */
	bit = (vdd - 2000) / 100 + 8;
	if (bit > max_bit)
		return max_bit;
	return bit;
}

/**
 * mmc_vddrange_to_ocrmask - Convert a voltage range to the OCR mask
 * @vdd_min:	minimum voltage value (mV)
 * @vdd_max:	maximum voltage value (mV)
 *
 * This function returns the OCR mask bits according to the provided @vdd_min
 * and @vdd_max values. If conversion is not possible the function returns 0.
 *
 * Notes wrt boundary cases:
 * This function sets the OCR bits for all boundary voltages, for example
 * [3300:3400] range is translated to MMC_VDD_32_33 | MMC_VDD_33_34 |
 * MMC_VDD_34_35 mask.
 */
u32 mmc_vddrange_to_ocrmask(int vdd_min, int vdd_max)
{
	u32 mask = 0;

	if (vdd_max < vdd_min)
		return 0;

	/* Prefer high bits for the boundary vdd_max values. */
	vdd_max = mmc_vdd_to_ocrbitnum(vdd_max, false);
	if (vdd_max < 0)
		return 0;

	/* Prefer low bits for the boundary vdd_min values. */
	vdd_min = mmc_vdd_to_ocrbitnum(vdd_min, true);
	if (vdd_min < 0)
		return 0;

	/* Fill the mask, from max bit to min bit. */
	while (vdd_max >= vdd_min)
		mask |= 1 << vdd_max--;

	return mask;
}
EXPORT_SYMBOL(mmc_vddrange_to_ocrmask);

#ifdef CONFIG_REGULATOR

/**
 * mmc_regulator_get_ocrmask - return mask of supported voltages
 * @supply: regulator to use
 *
 * This returns either a negative errno, or a mask of voltages that
 * can be provided to MMC/SD/SDIO devices using the specified voltage
 * regulator.  This would normally be called before registering the
 * MMC host adapter.
 */
int mmc_regulator_get_ocrmask(struct regulator *supply)
{
	int			result = 0;
	int			count;
	int			i;

	count = regulator_count_voltages(supply);
	if (count < 0)
		return count;

	for (i = 0; i < count; i++) {
		int		vdd_uV;
		int		vdd_mV;

		vdd_uV = regulator_list_voltage(supply, i);
		if (vdd_uV <= 0)
			continue;

		vdd_mV = vdd_uV / 1000;
		result |= mmc_vddrange_to_ocrmask(vdd_mV, vdd_mV);
	}

	return result;
}
EXPORT_SYMBOL(mmc_regulator_get_ocrmask);

/**
 * mmc_regulator_set_ocr - set regulator to match host->ios voltage
 * @mmc: the host to regulate
 * @supply: regulator to use
 * @vdd_bit: zero for power off, else a bit number (host->ios.vdd)
 *
 * Returns zero on success, else negative errno.
 *
 * MMC host drivers may use this to enable or disable a regulator using
 * a particular supply voltage.  This would normally be called from the
 * set_ios() method.
 */
int mmc_regulator_set_ocr(struct mmc_host *mmc,
			struct regulator *supply,
			unsigned short vdd_bit)
{
	int			result = 0;
	int			min_uV, max_uV;

	if (vdd_bit) {
		int		tmp;
		int		voltage;

		/* REVISIT mmc_vddrange_to_ocrmask() may have set some
		 * bits this regulator doesn't quite support ... don't
		 * be too picky, most cards and regulators are OK with
		 * a 0.1V range goof (it's a small error percentage).
		 */
		tmp = vdd_bit - ilog2(MMC_VDD_165_195);
		if (tmp == 0) {
			min_uV = 1650 * 1000;
			max_uV = 1950 * 1000;
		} else {
			min_uV = 1900 * 1000 + tmp * 100 * 1000;
			max_uV = min_uV + 100 * 1000;
		}

		/* avoid needless changes to this voltage; the regulator
		 * might not allow this operation
		 */
		voltage = regulator_get_voltage(supply);

		if (mmc->caps2 & MMC_CAP2_BROKEN_VOLTAGE)
			min_uV = max_uV = voltage;

		if (voltage < 0)
			result = voltage;
		else if (voltage < min_uV || voltage > max_uV)
			result = regulator_set_voltage(supply, min_uV, max_uV);
		else
			result = 0;

		if (result == 0 && !mmc->regulator_enabled) {
			result = regulator_enable(supply);
			if (!result)
				mmc->regulator_enabled = true;
		}
	} else if (mmc->regulator_enabled) {
		result = regulator_disable(supply);
		if (result == 0)
			mmc->regulator_enabled = false;
	}

	if (result)
		dev_err(mmc_dev(mmc),
			"could not set regulator OCR (%d)\n", result);
	return result;
}
EXPORT_SYMBOL(mmc_regulator_set_ocr);

#endif /* CONFIG_REGULATOR */

/*
 * Mask off any voltages we don't support and select
 * the lowest voltage
 */
u32 mmc_select_voltage(struct mmc_host *host, u32 ocr)
{
	int bit;

	ocr &= host->ocr_avail;

	bit = ffs(ocr);
	if (bit) {
		bit -= 1;

		ocr &= 3 << bit;

		mmc_host_clk_hold(host);
		host->ios.vdd = bit;
		mmc_set_ios(host);
		mmc_host_clk_release(host);
	} else {
		pr_warning("%s: host doesn't support card's voltages\n",
				mmc_hostname(host));
		ocr = 0;
	}

	return ocr;
}

int mmc_set_signal_voltage(struct mmc_host *host, int signal_voltage, bool cmd11)
{
	struct mmc_command cmd = {0};
	int err = 0;

	BUG_ON(!host);

	/* Some devices use only dedicated specific signaling level for
	 * design reasons. MMC_CAP2_BROKEN_VOLTAGE flag is used when
	 * there is no needs to change to any signaling level.
	 */
	if (host->caps2 & MMC_CAP2_BROKEN_VOLTAGE)
		return 0;

	/*
	 * Send CMD11 only if the request is to switch the card to
	 * 1.8V signalling.
	 */
	if ((signal_voltage != MMC_SIGNAL_VOLTAGE_330) && cmd11) {
		cmd.opcode = SD_SWITCH_VOLTAGE;
		cmd.arg = 0;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

		err = mmc_wait_for_cmd(host, &cmd, 0);
		if (err)
			return err;

		if (!mmc_host_is_spi(host) && (cmd.resp[0] & R1_ERROR))
			return -EIO;
	}

	host->ios.signal_voltage = signal_voltage;

	if (host->ops->start_signal_voltage_switch) {
		mmc_host_clk_hold(host);
		err = host->ops->start_signal_voltage_switch(host, &host->ios);
		mmc_host_clk_release(host);
	}

	return err;
}

/*
 * Select timing parameters for host.
 */
void mmc_set_timing(struct mmc_host *host, unsigned int timing)
{
	mmc_host_clk_hold(host);
	host->ios.timing = timing;
	mmc_set_ios(host);
	mmc_host_clk_release(host);
}

/*
 * Select appropriate driver type for host.
 */
void mmc_set_driver_type(struct mmc_host *host, unsigned int drv_type)
{
	mmc_host_clk_hold(host);
	host->ios.drv_type = drv_type;
	mmc_set_ios(host);
	mmc_host_clk_release(host);
}

void mmc_poweroff_notify(struct mmc_host *host)
{
	struct mmc_card *card;
	unsigned int timeout;
	unsigned int notify_type = EXT_CSD_NO_POWER_NOTIFICATION;
	int err = 0;

	card = host->card;
	mmc_claim_host(host);

	/*
	 * Send power notify command only if card
	 * is mmc and notify state is powered ON
	 */
	if (card && mmc_card_mmc(card) &&
	    (card->poweroff_notify_state == MMC_POWERED_ON)) {

		if (host->power_notify_type == MMC_HOST_PW_NOTIFY_SHORT) {
			notify_type = EXT_CSD_POWER_OFF_SHORT;
			timeout = card->ext_csd.generic_cmd6_time;
			card->poweroff_notify_state = MMC_POWEROFF_SHORT;
		} else {
			notify_type = EXT_CSD_POWER_OFF_LONG;
			timeout = card->ext_csd.power_off_longtime;
			card->poweroff_notify_state = MMC_POWEROFF_LONG;
		}

		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_POWER_OFF_NOTIFICATION,
				 notify_type, timeout);

		if (err && err != -EBADMSG)
			pr_err("Device failed to respond within %d poweroff "
			       "time. Forcefully powering down the device\n",
			       timeout);

		/* Set the card state to no notification after the poweroff */
		card->poweroff_notify_state = MMC_NO_POWER_NOTIFICATION;
	}
	mmc_release_host(host);
}
EXPORT_SYMBOL(mmc_poweroff_notify);

/*
 * Apply power to the MMC stack.  This is a two-stage process.
 * First, we enable power to the card without the clock running.
 * We then wait a bit for the power to stabilise.  Finally,
 * enable the bus drivers and clock to the card.
 *
 * We must _NOT_ enable the clock prior to power stablising.
 *
 * If a host does all the power sequencing itself, ignore the
 * initial MMC_POWER_UP stage.
 */
static void mmc_power_up(struct mmc_host *host)
{
	int bit;

	mmc_host_clk_hold(host);

	/* If ocr is set, we use it */
	if (host->ocr)
		bit = ffs(host->ocr) - 1;
	else
		bit = fls(host->ocr_avail) - 1;

	host->ios.vdd = bit;
	if (mmc_host_is_spi(host))
		host->ios.chip_select = MMC_CS_HIGH;
	else
		host->ios.chip_select = MMC_CS_DONTCARE;
	host->ios.bus_mode = MMC_BUSMODE_PUSHPULL;
	host->ios.power_mode = MMC_POWER_UP;
	host->ios.bus_width = MMC_BUS_WIDTH_1;
	host->ios.timing = MMC_TIMING_LEGACY;
	mmc_set_ios(host);

	/*
	 * This delay should be sufficient to allow the power supply
	 * to reach the minimum voltage.
	 */
	mmc_delay(10);

	host->ios.clock = host->f_init;

	host->ios.power_mode = MMC_POWER_ON;
	mmc_set_ios(host);

	/*
	 * This delay must be at least 74 clock sizes, or 1 ms, or the
	 * time required to reach a stable voltage.
	 */
	mmc_delay(10);

	mmc_host_clk_release(host);
}

void mmc_power_off(struct mmc_host *host)
{
	int err = 0;
	mmc_host_clk_hold(host);

	host->ios.clock = 0;
	host->ios.vdd = 0;

	/*
	 * For eMMC 4.5 device send AWAKE command before
	 * POWER_OFF_NOTIFY command, because in sleep state
	 * eMMC 4.5 devices respond to only RESET and AWAKE cmd
	 */
	if (host->card && mmc_card_is_sleep(host->card) &&
	    host->bus_ops->resume) {
		err = host->bus_ops->resume(host);

		if (!err)
			mmc_poweroff_notify(host);
		else
			pr_warning("%s: error %d during resume "
				   "(continue with poweroff sequence)\n",
				   mmc_hostname(host), err);
	}

	/*
	 * Reset ocr mask to be the highest possible voltage supported for
	 * this mmc host. This value will be used at next power up.
	 */
	host->ocr = 1 << (fls(host->ocr_avail) - 1);

	if (!mmc_host_is_spi(host)) {
		host->ios.bus_mode = MMC_BUSMODE_OPENDRAIN;
		host->ios.chip_select = MMC_CS_DONTCARE;
	}
	host->ios.power_mode = MMC_POWER_OFF;
	host->ios.bus_width = MMC_BUS_WIDTH_1;
	host->ios.timing = MMC_TIMING_LEGACY;
	mmc_set_ios(host);

	/*
	 * Some configurations, such as the 802.11 SDIO card in the OLPC
	 * XO-1.5, require a short delay after poweroff before the card
	 * can be successfully turned on again.
	 */
	mmc_delay(1);

	mmc_host_clk_release(host);
}

/*
 * Cleanup when the last reference to the bus operator is dropped.
 */
static void __mmc_release_bus(struct mmc_host *host)
{
	BUG_ON(!host);
	BUG_ON(host->bus_refs);
	BUG_ON(!host->bus_dead);

	host->bus_ops = NULL;
}

/*
 * Increase reference count of bus operator
 */
static inline void mmc_bus_get(struct mmc_host *host)
{
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	host->bus_refs++;
	spin_unlock_irqrestore(&host->lock, flags);
}

/*
 * Decrease reference count of bus operator and free it if
 * it is the last reference.
 */
static inline void mmc_bus_put(struct mmc_host *host)
{
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	host->bus_refs--;
	if ((host->bus_refs == 0) && host->bus_ops)
		__mmc_release_bus(host);
	spin_unlock_irqrestore(&host->lock, flags);
}

int mmc_resume_bus(struct mmc_host *host)
{
	unsigned long flags;

	if (!mmc_bus_needs_resume(host))
		return -EINVAL;

	printk("%s: Starting deferred resume\n", mmc_hostname(host));
	spin_lock_irqsave(&host->lock, flags);
	host->bus_resume_flags &= ~MMC_BUSRESUME_NEEDS_RESUME;
	host->rescan_disable = 0;
	spin_unlock_irqrestore(&host->lock, flags);

	mmc_bus_get(host);
	if (host->bus_ops && !host->bus_dead) {
		mmc_power_up(host);
		BUG_ON(!host->bus_ops->resume);
		host->bus_ops->resume(host);
		if (host->bus_ops->detect)
			host->bus_ops->detect(host);
	}

	mmc_bus_put(host);
	printk("%s: Deferred resume completed\n", mmc_hostname(host));
	return 0;
}

EXPORT_SYMBOL(mmc_resume_bus);

/*
 * Assign a mmc bus handler to a host. Only one bus handler may control a
 * host at any given time.
 */
void mmc_attach_bus(struct mmc_host *host, const struct mmc_bus_ops *ops)
{
	unsigned long flags;

	BUG_ON(!host);
	BUG_ON(!ops);

	WARN_ON(!host->claimed);

	spin_lock_irqsave(&host->lock, flags);

	BUG_ON(host->bus_ops);
	BUG_ON(host->bus_refs);

	host->bus_ops = ops;
	host->bus_refs = 1;
	host->bus_dead = 0;

	spin_unlock_irqrestore(&host->lock, flags);
}

/*
 * Remove the current bus handler from a host.
 */
void mmc_detach_bus(struct mmc_host *host)
{
	unsigned long flags;

	BUG_ON(!host);

	WARN_ON(!host->claimed);
	WARN_ON(!host->bus_ops);

	spin_lock_irqsave(&host->lock, flags);

	host->bus_dead = 1;

	spin_unlock_irqrestore(&host->lock, flags);

	mmc_bus_put(host);
}

/**
 *	mmc_detect_change - process change of state on a MMC socket
 *	@host: host which changed state.
 *	@delay: optional delay to wait before detection (jiffies)
 *
 *	MMC drivers should call this when they detect a card has been
 *	inserted or removed. The MMC layer will confirm that any
 *	present card is still functional, and initialize any newly
 *	inserted.
 */
void mmc_detect_change(struct mmc_host *host, unsigned long delay)
{
#ifdef CONFIG_MMC_DEBUG
	unsigned long flags;
	spin_lock_irqsave(&host->lock, flags);
	WARN_ON(host->removed);
	spin_unlock_irqrestore(&host->lock, flags);
#endif
	host->detect_change = 1;

	wake_lock(&host->detect_wake_lock);
	mmc_schedule_delayed_work(&host->detect, delay);
}

EXPORT_SYMBOL(mmc_detect_change);

void mmc_init_erase(struct mmc_card *card)
{
	unsigned int sz;

	if (is_power_of_2(card->erase_size))
		card->erase_shift = ffs(card->erase_size) - 1;
	else
		card->erase_shift = 0;

	/*
	 * It is possible to erase an arbitrarily large area of an SD or MMC
	 * card.  That is not desirable because it can take a long time
	 * (minutes) potentially delaying more important I/O, and also the
	 * timeout calculations become increasingly hugely over-estimated.
	 * Consequently, 'pref_erase' is defined as a guide to limit erases
	 * to that size and alignment.
	 *
	 * For SD cards that define Allocation Unit size, limit erases to one
	 * Allocation Unit at a time.  For MMC cards that define High Capacity
	 * Erase Size, whether it is switched on or not, limit to that size.
	 * Otherwise just have a stab at a good value.  For modern cards it
	 * will end up being 4MiB.  Note that if the value is too small, it
	 * can end up taking longer to erase.
	 */
	if (mmc_card_sd(card) && card->ssr.au) {
		card->pref_erase = card->ssr.au;
		card->erase_shift = ffs(card->ssr.au) - 1;
	} else if (card->ext_csd.hc_erase_size) {
		card->pref_erase = card->ext_csd.hc_erase_size;
	} else {
		sz = (card->csd.capacity << (card->csd.read_blkbits - 9)) >> 11;
		if (sz < 128)
			card->pref_erase = 512 * 1024 / 512;
		else if (sz < 512)
			card->pref_erase = 1024 * 1024 / 512;
		else if (sz < 1024)
			card->pref_erase = 2 * 1024 * 1024 / 512;
		else
			card->pref_erase = 4 * 1024 * 1024 / 512;
		if (card->pref_erase < card->erase_size)
			card->pref_erase = card->erase_size;
		else {
			sz = card->pref_erase % card->erase_size;
			if (sz)
				card->pref_erase += card->erase_size - sz;
		}
	}
}

static unsigned int mmc_mmc_erase_timeout(struct mmc_card *card,
				          unsigned int arg, unsigned int qty)
{
	unsigned int erase_timeout;

	if (arg == MMC_DISCARD_ARG ||
	    (arg == MMC_TRIM_ARG && card->ext_csd.rev >= 6)) {
		erase_timeout = card->ext_csd.trim_timeout;
	} else if (card->ext_csd.erase_group_def & 1) {
		/* High Capacity Erase Group Size uses HC timeouts */
		if (arg == MMC_TRIM_ARG)
			erase_timeout = card->ext_csd.trim_timeout;
		else
			erase_timeout = card->ext_csd.hc_erase_timeout;
	} else {
		/* CSD Erase Group Size uses write timeout */
		unsigned int mult = (10 << card->csd.r2w_factor);
		unsigned int timeout_clks = card->csd.tacc_clks * mult;
		unsigned int timeout_us;

		/* Avoid overflow: e.g. tacc_ns=80000000 mult=1280 */
		if (card->csd.tacc_ns < 1000000)
			timeout_us = (card->csd.tacc_ns * mult) / 1000;
		else
			timeout_us = (card->csd.tacc_ns / 1000) * mult;

		/*
		 * ios.clock is only a target.  The real clock rate might be
		 * less but not that much less, so fudge it by multiplying by 2.
		 */
		timeout_clks <<= 1;
		timeout_us += (timeout_clks * 1000) /
			      (mmc_host_clk_rate(card->host) / 1000);

		erase_timeout = timeout_us / 1000;

		/*
		 * Theoretically, the calculation could underflow so round up
		 * to 1ms in that case.
		 */
		if (!erase_timeout)
			erase_timeout = 1;
	}

	/* Multiplier for secure operations */
	if (arg & MMC_SECURE_ARGS) {
		if (arg == MMC_SECURE_ERASE_ARG)
			erase_timeout *= card->ext_csd.sec_erase_mult;
		else
			erase_timeout *= card->ext_csd.sec_trim_mult;
	}

	erase_timeout *= qty;

	/*
	 * Ensure at least a 1 second timeout for SPI as per
	 * 'mmc_set_data_timeout()'
	 */
	if (mmc_host_is_spi(card->host) && erase_timeout < 1000)
		erase_timeout = 1000;

	return erase_timeout;
}

static unsigned int mmc_sd_erase_timeout(struct mmc_card *card,
					 unsigned int arg,
					 unsigned int qty)
{
	unsigned int erase_timeout;

	if (card->ssr.erase_timeout) {
		/* Erase timeout specified in SD Status Register (SSR) */
		erase_timeout = card->ssr.erase_timeout * qty +
				card->ssr.erase_offset;
	} else {
		/*
		 * Erase timeout not specified in SD Status Register (SSR) so
		 * use 250ms per write block.
		 */
		erase_timeout = 250 * qty;
	}

	/* Must not be less than 1 second */
	if (erase_timeout < 1000)
		erase_timeout = 1000;

	return erase_timeout;
}

static unsigned int mmc_erase_timeout(struct mmc_card *card,
				      unsigned int arg,
				      unsigned int qty)
{
	if (mmc_card_sd(card))
		return mmc_sd_erase_timeout(card, arg, qty);
	else
		return mmc_mmc_erase_timeout(card, arg, qty);
}

static int mmc_do_erase(struct mmc_card *card, unsigned int from,
			unsigned int to, unsigned int arg)
{
	struct mmc_command cmd = {0};
	unsigned int qty = 0;
	int err;

	mmc_add_trace(__MMC_TA_PRE_DONE, card->host->mqrq_cur);
	/*
	 * qty is used to calculate the erase timeout which depends on how many
	 * erase groups (or allocation units in SD terminology) are affected.
	 * We count erasing part of an erase group as one erase group.
	 * For SD, the allocation units are always a power of 2.  For MMC, the
	 * erase group size is almost certainly also power of 2, but it does not
	 * seem to insist on that in the JEDEC standard, so we fall back to
	 * division in that case.  SD may not specify an allocation unit size,
	 * in which case the timeout is based on the number of write blocks.
	 *
	 * Note that the timeout for secure trim 2 will only be correct if the
	 * number of erase groups specified is the same as the total of all
	 * preceding secure trim 1 commands.  Since the power may have been
	 * lost since the secure trim 1 commands occurred, it is generally
	 * impossible to calculate the secure trim 2 timeout correctly.
	 */
	if (card->erase_shift)
		qty += ((to >> card->erase_shift) -
			(from >> card->erase_shift)) + 1;
	else if (mmc_card_sd(card))
		qty += to - from + 1;
	else
		qty += ((to / card->erase_size) -
			(from / card->erase_size)) + 1;

	if (!mmc_card_blockaddr(card)) {
		from <<= 9;
		to <<= 9;
	}

	if (mmc_card_sd(card))
		cmd.opcode = SD_ERASE_WR_BLK_START;
	else
		cmd.opcode = MMC_ERASE_GROUP_START;
	cmd.arg = from;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err) {
		pr_err("mmc_erase: group start error %d, "
		       "status %#x\n", err, cmd.resp[0]);
		err = -EIO;
		goto out;
	}

	memset(&cmd, 0, sizeof(struct mmc_command));
	if (mmc_card_sd(card))
		cmd.opcode = SD_ERASE_WR_BLK_END;
	else
		cmd.opcode = MMC_ERASE_GROUP_END;
	cmd.arg = to;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err) {
		pr_err("mmc_erase: group end error %d, status %#x\n",
		       err, cmd.resp[0]);
		err = -EIO;
		goto out;
	}

	memset(&cmd, 0, sizeof(struct mmc_command));
	cmd.opcode = MMC_ERASE;
	cmd.arg = arg;
	cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
	cmd.cmd_timeout_ms = mmc_erase_timeout(card, arg, qty);

	mmc_add_trace(__MMC_TA_MMC_ISSUE, card->host->mqrq_cur);
	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err) {
		pr_err("mmc_erase: erase error %d, status %#x\n",
		       err, cmd.resp[0]);
		err = -EIO;
		goto out;
	}

	mmc_add_trace(__MMC_TA_MMC_DONE, card->host->mqrq_cur);
	if (mmc_host_is_spi(card->host))
		goto out;

	do {
		memset(&cmd, 0, sizeof(struct mmc_command));
		cmd.opcode = MMC_SEND_STATUS;
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;
		/* Do not retry else we can't see errors */
		err = mmc_wait_for_cmd(card->host, &cmd, 0);
		if (err || (cmd.resp[0] & 0xFDF92000)) {
			pr_err("error %d requesting status %#x\n",
				err, cmd.resp[0]);
			err = -EIO;
			goto out;
		}
	} while (!(cmd.resp[0] & R1_READY_FOR_DATA) ||
		 R1_CURRENT_STATE(cmd.resp[0]) == R1_STATE_PRG);

	mmc_add_trace(__MMC_TA_REQ_DONE, card->host->mqrq_cur);
out:
	return err;
}

/**
 * mmc_erase - erase sectors.
 * @card: card to erase
 * @from: first sector to erase
 * @nr: number of sectors to erase
 * @arg: erase command argument (SD supports only %MMC_ERASE_ARG)
 *
 * Caller must claim host before calling this function.
 */
int mmc_erase(struct mmc_card *card, unsigned int from, unsigned int nr,
	      unsigned int arg)
{
	unsigned int rem, to = from + nr;

	if (!(card->host->caps & MMC_CAP_ERASE) ||
	    !(card->csd.cmdclass & CCC_ERASE))
		return -EOPNOTSUPP;

	if (!card->erase_size)
		return -EOPNOTSUPP;

	if (mmc_card_sd(card) && arg != MMC_ERASE_ARG)
		return -EOPNOTSUPP;

	if ((arg & MMC_SECURE_ARGS) &&
	    !(card->ext_csd.sec_feature_support & EXT_CSD_SEC_ER_EN))
		return -EOPNOTSUPP;

	if ((arg & MMC_TRIM_ARGS) &&
	    !(card->ext_csd.sec_feature_support & EXT_CSD_SEC_GB_CL_EN))
		return -EOPNOTSUPP;

	if (arg == MMC_SECURE_ERASE_ARG) {
		if (from % card->erase_size || nr % card->erase_size)
			return -EINVAL;
	}

	if (arg == MMC_ERASE_ARG) {
		rem = from % card->erase_size;
		if (rem) {
			rem = card->erase_size - rem;
			from += rem;
			if (nr > rem)
				nr -= rem;
			else
				return 0;
		}
		rem = nr % card->erase_size;
		if (rem)
			nr -= rem;
	}

	if (nr == 0)
		return 0;

	to = from + nr;

	if (to <= from)
		return -EINVAL;

	/* to set the address in 16k (32sectors) */
	if(arg == MMC_TRIM_ARG) {
		if ((from % 32) != 0)
			from = ((from >> 5) + 1) << 5;

		to = (to >> 5) << 5;
		if (from >= to)
			return 0;
	}

	/* 'from' and 'to' are inclusive */
	to -= 1;

	return mmc_do_erase(card, from, to, arg);
}
EXPORT_SYMBOL(mmc_erase);

int mmc_can_erase(struct mmc_card *card)
{
	if ((card->host->caps & MMC_CAP_ERASE) &&
	    (card->csd.cmdclass & CCC_ERASE) && card->erase_size)
		return 1;
	return 0;
}
EXPORT_SYMBOL(mmc_can_erase);

int mmc_can_trim(struct mmc_card *card)
{
	if (card->ext_csd.sec_feature_support & EXT_CSD_SEC_GB_CL_EN)
		return 1;
	return 0;
}
EXPORT_SYMBOL(mmc_can_trim);

int mmc_can_discard(struct mmc_card *card)
{
	/*
	 * As there's no way to detect the discard support bit at v4.5
	 * use the s/w feature support filed.
	 */
	if (card->ext_csd.feature_support & MMC_DISCARD_FEATURE)
		return 1;
	return 0;
}
EXPORT_SYMBOL(mmc_can_discard);

int mmc_can_sanitize(struct mmc_card *card)
{
	if (!mmc_can_trim(card) && !mmc_can_erase(card))
		return 0;
	if (card->ext_csd.sec_feature_support & EXT_CSD_SEC_SANITIZE)
		return 1;
	return 0;
}
EXPORT_SYMBOL(mmc_can_sanitize);

int mmc_can_secure_erase_trim(struct mmc_card *card)
{
	if (card->ext_csd.sec_feature_support & EXT_CSD_SEC_ER_EN)
		return 1;
	return 0;
}
EXPORT_SYMBOL(mmc_can_secure_erase_trim);

int mmc_erase_group_aligned(struct mmc_card *card, unsigned int from,
			    unsigned int nr)
{
	if (!card->erase_size)
		return 0;
	if (from % card->erase_size || nr % card->erase_size)
		return 0;
	return 1;
}
EXPORT_SYMBOL(mmc_erase_group_aligned);

static unsigned int mmc_do_calc_max_discard(struct mmc_card *card,
					    unsigned int arg)
{
	struct mmc_host *host = card->host;
	unsigned int max_discard, x, y, qty = 0, max_qty, timeout;
	unsigned int last_timeout = 0;

	if (card->erase_shift)
		max_qty = UINT_MAX >> card->erase_shift;
	else if (mmc_card_sd(card))
		max_qty = UINT_MAX;
	else
		max_qty = UINT_MAX / card->erase_size;

	/* Find the largest qty with an OK timeout */
	do {
		y = 0;
		for (x = 1; x && x <= max_qty && max_qty - x >= qty; x <<= 1) {
			timeout = mmc_erase_timeout(card, arg, qty + x);
			if (timeout > host->max_discard_to)
				break;
			if (timeout < last_timeout)
				break;
			last_timeout = timeout;
			y = x;
		}
		qty += y;
	} while (y);

	if (!qty)
		return 0;

	if (qty == 1)
		return 1;

	/* Convert qty to sectors */
	if (card->erase_shift)
		max_discard = --qty << card->erase_shift;
	else if (mmc_card_sd(card))
		max_discard = qty;
	else
		max_discard = --qty * card->erase_size;

	return max_discard;
}

unsigned int mmc_calc_max_discard(struct mmc_card *card)
{
	struct mmc_host *host = card->host;
	unsigned int max_discard, max_trim;

	if (!host->max_discard_to)
		return UINT_MAX;

	/*
	 * Without erase_group_def set, MMC erase timeout depends on clock
	 * frequence which can change.  In that case, the best choice is
	 * just the preferred erase size.
	 */
	if (mmc_card_mmc(card) && !(card->ext_csd.erase_group_def & 1))
		return card->pref_erase;

	max_discard = mmc_do_calc_max_discard(card, MMC_ERASE_ARG);
	if (mmc_can_trim(card)) {
		max_trim = mmc_do_calc_max_discard(card, MMC_TRIM_ARG);
		if (max_trim < max_discard)
			max_discard = max_trim;
	} else if (max_discard < card->erase_size) {
		max_discard = 0;
	}
	pr_debug("%s: calculated max. discard sectors %u for timeout %u ms\n",
		 mmc_hostname(host), max_discard, host->max_discard_to);
	return max_discard;
}
EXPORT_SYMBOL(mmc_calc_max_discard);

int mmc_set_blocklen(struct mmc_card *card, unsigned int blocklen)
{
	struct mmc_command cmd = {0};

	if (mmc_card_blockaddr(card) || mmc_card_ddr_mode(card))
		return 0;

	cmd.opcode = MMC_SET_BLOCKLEN;
	cmd.arg = blocklen;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	return mmc_wait_for_cmd(card->host, &cmd, 5);
}
EXPORT_SYMBOL(mmc_set_blocklen);

static void mmc_hw_reset_for_init(struct mmc_host *host)
{
	if (!(host->caps & MMC_CAP_HW_RESET) || !host->ops->hw_reset)
		return;
	mmc_host_clk_hold(host);
	host->ops->hw_reset(host);
	mmc_host_clk_release(host);
}

int mmc_can_reset(struct mmc_card *card)
{
	u8 rst_n_function;

	if (!mmc_card_mmc(card))
		return 0;
	rst_n_function = card->ext_csd.rst_n_function;
	if ((rst_n_function & EXT_CSD_RST_N_EN_MASK) != EXT_CSD_RST_N_ENABLED)
		return 0;
	return 1;
}
EXPORT_SYMBOL(mmc_can_reset);

static int mmc_do_hw_reset(struct mmc_host *host, int check)
{
	struct mmc_card *card = host->card;

	if (!host->bus_ops->power_restore)
		return -EOPNOTSUPP;

	if (!(host->caps & MMC_CAP_HW_RESET) || !host->ops->hw_reset)
		return -EOPNOTSUPP;

	if (!card)
		return -EINVAL;

	if (!mmc_can_reset(card))
		return -EOPNOTSUPP;

	mmc_host_clk_hold(host);
	mmc_set_clock(host, host->f_init);

	host->ops->hw_reset(host);

	/* If the reset has happened, then a status command will fail */
	if (check) {
		struct mmc_command cmd = {0};
		int err;

		cmd.opcode = MMC_SEND_STATUS;
		if (!mmc_host_is_spi(card->host))
			cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;
		err = mmc_wait_for_cmd(card->host, &cmd, 0);
		if (!err) {
			mmc_host_clk_release(host);
			return -ENOSYS;
		}
	}

	host->card->state &= ~(MMC_STATE_HIGHSPEED | MMC_STATE_HIGHSPEED_DDR);
	if (mmc_host_is_spi(host)) {
		host->ios.chip_select = MMC_CS_HIGH;
		host->ios.bus_mode = MMC_BUSMODE_PUSHPULL;
	} else {
		host->ios.chip_select = MMC_CS_DONTCARE;
		host->ios.bus_mode = MMC_BUSMODE_OPENDRAIN;
	}
	host->ios.bus_width = MMC_BUS_WIDTH_1;
	host->ios.timing = MMC_TIMING_LEGACY;
	mmc_set_ios(host);

	mmc_host_clk_release(host);

	return host->bus_ops->power_restore(host);
}

int mmc_hw_reset(struct mmc_host *host)
{
	return mmc_do_hw_reset(host, 0);
}
EXPORT_SYMBOL(mmc_hw_reset);

int mmc_hw_reset_check(struct mmc_host *host)
{
	return mmc_do_hw_reset(host, 1);
}
EXPORT_SYMBOL(mmc_hw_reset_check);

static int mmc_rescan_try_freq(struct mmc_host *host, unsigned freq)
{
	host->f_init = freq;

#ifdef CONFIG_MMC_DEBUG
	pr_info("%s: %s: trying to init card at %u Hz\n",
		mmc_hostname(host), __func__, host->f_init);
#endif
	mmc_power_up(host);

	/*
	 * Some eMMCs (with VCCQ always on) may not be reset after power up, so
	 * do a hardware reset if possible.
	 */
	mmc_hw_reset_for_init(host);

	/* Initialization should be done at 3.3 V I/O voltage. */
	mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_330, 0);

	/*
	 * sdio_reset sends CMD52 to reset card.  Since we do not know
	 * if the card is being re-initialized, just send it.  CMD52
	 * should be ignored by SD/eMMC cards.
	 */
	sdio_reset(host);
	mmc_go_idle(host);

	mmc_send_if_cond(host, host->ocr_avail);

	/* Order's important: probe SDIO, then SD, then MMC */
	if (!mmc_attach_sdio(host))
		return 0;
	if (!mmc_attach_sd(host))
		return 0;
	if (!mmc_attach_mmc(host))
		return 0;

	mmc_power_off(host);
	return -EIO;
}

int _mmc_detect_card_removed(struct mmc_host *host)
{
	int ret;

	if ((host->caps & MMC_CAP_NONREMOVABLE) || !host->bus_ops->alive)
		return 0;

	if (!host->card || mmc_card_removed(host->card))
		return 1;

	ret = host->bus_ops->alive(host);
	if (ret) {
		mmc_card_set_removed(host->card);
		pr_debug("%s: card remove detected\n", mmc_hostname(host));
	}

	return ret;
}

int mmc_detect_card_removed(struct mmc_host *host)
{
	struct mmc_card *card = host->card;
	int ret;

	WARN_ON(!host->claimed);

	if (!card)
		return 1;

	ret = mmc_card_removed(card);
	/*
	 * The card will be considered unchanged unless we have been asked to
	 * detect a change or host requires polling to provide card detection.
	 */
	if (!host->detect_change && !(host->caps & MMC_CAP_NEEDS_POLL) &&
	    !(host->caps2 & MMC_CAP2_DETECT_ON_ERR))
		return ret;

	host->detect_change = 0;
	if (!ret) {
		ret = _mmc_detect_card_removed(host);
		if (ret && (host->caps2 & MMC_CAP2_DETECT_ON_ERR)) {
			/*
			 * Schedule a detect work as soon as possible to let a
			 * rescan handle the card removal.
			 */
			cancel_delayed_work(&host->detect);
			mmc_detect_change(host, 0);
		}
	}

	return ret;
}
EXPORT_SYMBOL(mmc_detect_card_removed);

void mmc_rescan(struct work_struct *work)
{
	static const unsigned freqs[] = { 400000, 300000, 200000, 100000 };
	struct mmc_host *host =
		container_of(work, struct mmc_host, detect.work);
	int i;
	bool extend_wakelock = false;

	if (host->rescan_disable)
		return;

	mmc_bus_get(host);

	/*
	 * if there is a _removable_ card registered, check whether it is
	 * still present
	 */
	if (host->bus_ops && host->bus_ops->detect && !host->bus_dead
	    && !(host->caps & MMC_CAP_NONREMOVABLE))
		host->bus_ops->detect(host);

	host->detect_change = 0;

	/* If the card was removed the bus will be marked
	 * as dead - extend the wakelock so userspace
	 * can respond */
	if (host->bus_dead)
		extend_wakelock = 1;

	/*
	 * Let mmc_bus_put() free the bus/bus_ops if we've found that
	 * the card is no longer present.
	 */
	mmc_bus_put(host);
	mmc_bus_get(host);

	/* if there still is a card present, stop here */
	if (host->bus_ops != NULL) {
		mmc_bus_put(host);
		goto out;
	}

	/*
	 * Only we can add a new handler, so it's safe to
	 * release the lock here.
	 */
	mmc_bus_put(host);

	if (host->ops->get_cd && host->ops->get_cd(host) == 0)
		goto out;

	mmc_claim_host(host);
	for (i = 0; i < ARRAY_SIZE(freqs); i++) {
		if (!mmc_rescan_try_freq(host, max(freqs[i], host->f_min))) {
			extend_wakelock = true;
			break;
		}
		if (freqs[i] <= host->f_min)
			break;
	}
	mmc_release_host(host);

 out:
	if (extend_wakelock)
		wake_lock_timeout(&host->detect_wake_lock, HZ / 2);
	else
		wake_unlock(&host->detect_wake_lock);
	if (host->caps & MMC_CAP_NEEDS_POLL) {
		wake_lock(&host->detect_wake_lock);
		mmc_schedule_delayed_work(&host->detect, HZ);
	}
}

void mmc_start_host(struct mmc_host *host)
{
	mmc_power_off(host);
	mmc_detect_change(host, 0);
}

void mmc_stop_host(struct mmc_host *host)
{
#ifdef CONFIG_MMC_DEBUG
	unsigned long flags;
	spin_lock_irqsave(&host->lock, flags);
	host->removed = 1;
	spin_unlock_irqrestore(&host->lock, flags);
#endif

	if (cancel_delayed_work_sync(&host->detect))
		wake_unlock(&host->detect_wake_lock);
	mmc_flush_scheduled_work();

	/* clear pm flags now and let card drivers set them as needed */
	host->pm_flags = 0;

	mmc_bus_get(host);
	if (host->bus_ops && !host->bus_dead) {
		/* Calling bus_ops->remove() with a claimed host can deadlock */
		if (host->bus_ops->remove)
			host->bus_ops->remove(host);

		mmc_claim_host(host);
		mmc_detach_bus(host);
		mmc_power_off(host);
		mmc_release_host(host);
		mmc_bus_put(host);
		return;
	}
	mmc_bus_put(host);

	BUG_ON(host->card);

	mmc_power_off(host);
}

int mmc_power_save_host(struct mmc_host *host)
{
	int ret = 0;

#ifdef CONFIG_MMC_DEBUG
	pr_info("%s: %s: powering down\n", mmc_hostname(host), __func__);
#endif

	mmc_bus_get(host);

	if (!host->bus_ops || host->bus_dead || !host->bus_ops->power_restore) {
		mmc_bus_put(host);
		return -EINVAL;
	}

	if (host->bus_ops->power_save)
		ret = host->bus_ops->power_save(host);

	mmc_bus_put(host);

	mmc_power_off(host);

	return ret;
}
EXPORT_SYMBOL(mmc_power_save_host);

int mmc_power_restore_host(struct mmc_host *host)
{
	int ret;

#ifdef CONFIG_MMC_DEBUG
	pr_info("%s: %s: powering up\n", mmc_hostname(host), __func__);
#endif

	mmc_bus_get(host);

	if (!host->bus_ops || host->bus_dead || !host->bus_ops->power_restore) {
		mmc_bus_put(host);
		return -EINVAL;
	}

	mmc_power_up(host);
	ret = host->bus_ops->power_restore(host);

	mmc_bus_put(host);

	return ret;
}
EXPORT_SYMBOL(mmc_power_restore_host);

int mmc_card_awake(struct mmc_host *host)
{
	int err = -ENOSYS;

	if (host->caps2 & MMC_CAP2_NO_SLEEP_CMD)
		return 0;

	mmc_bus_get(host);

	if (host->bus_ops && !host->bus_dead && host->bus_ops->awake)
		err = host->bus_ops->awake(host);

	mmc_bus_put(host);

	return err;
}
EXPORT_SYMBOL(mmc_card_awake);

int mmc_card_sleep(struct mmc_host *host)
{
	int err = -ENOSYS;

	if (host->caps2 & MMC_CAP2_NO_SLEEP_CMD)
		return 0;

	mmc_bus_get(host);

	if (host->bus_ops && !host->bus_dead && host->bus_ops->sleep)
		err = host->bus_ops->sleep(host);

	mmc_bus_put(host);

	return err;
}
EXPORT_SYMBOL(mmc_card_sleep);

int mmc_card_can_sleep(struct mmc_host *host)
{
	struct mmc_card *card = host->card;

	if (host->caps2 & MMC_CAP2_NO_SLEEP_CMD)
		return 0;
	if (card && mmc_card_mmc(card) && card->ext_csd.rev >= 3)
		return 1;
	return 0;
}
EXPORT_SYMBOL(mmc_card_can_sleep);

/*
 * Flush the cache to the non-volatile storage.
 */
int mmc_flush_cache(struct mmc_card *card)
{
	struct mmc_host *host = card->host;
	int err = 0;

	if (!(host->caps2 & MMC_CAP2_CACHE_CTRL))
		return err;

	mmc_add_trace(__MMC_TA_PRE_DONE, card->host->mqrq_cur);
	mmc_add_trace(__MMC_TA_MMC_ISSUE, card->host->mqrq_cur);

	if (mmc_card_mmc(card) &&
			(card->ext_csd.cache_size > 0) &&
			(card->ext_csd.cache_ctrl & 1)) {
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				EXT_CSD_FLUSH_CACHE, 1, 0);
		if (err)
			pr_err("%s: cache flush error %d\n",
					mmc_hostname(card->host), err);
	}

	mmc_add_trace(__MMC_TA_MMC_DONE, card->host->mqrq_cur);
	mmc_add_trace(__MMC_TA_REQ_DONE, card->host->mqrq_cur);

	return err;
}
EXPORT_SYMBOL(mmc_flush_cache);

/*
 * Turn the cache ON/OFF.
 * Turning the cache OFF shall trigger flushing of the data
 * to the non-volatile storage.
 */
int mmc_cache_ctrl(struct mmc_host *host, u8 enable)
{
	struct mmc_card *card = host->card;
	unsigned int timeout;
	int err = 0;

	if (!(host->caps2 & MMC_CAP2_CACHE_CTRL) ||
			mmc_card_is_removable(host))
		return err;

	mmc_claim_host(host);
	if (card && mmc_card_mmc(card) &&
			(card->ext_csd.cache_size > 0)) {
		enable = !!enable;

		if (card->ext_csd.cache_ctrl ^ enable) {
			timeout = enable ? card->ext_csd.generic_cmd6_time : 0;
			err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					EXT_CSD_CACHE_CTRL, enable, timeout);
			if (err)
				pr_err("%s: cache %s error %d\n",
						mmc_hostname(card->host),
						enable ? "on" : "off",
						err);
			else
				card->ext_csd.cache_ctrl = enable;
		}
	}
	mmc_release_host(host);

	return err;
}
EXPORT_SYMBOL(mmc_cache_ctrl);

#ifdef CONFIG_PM

/**
 *	mmc_suspend_host - suspend a host
 *	@host: mmc host
 */
int mmc_suspend_host(struct mmc_host *host)
{
	int err = 0;

	if (mmc_bus_needs_resume(host))
		return 0;

	if (cancel_delayed_work(&host->detect))
		wake_unlock(&host->detect_wake_lock);
	mmc_flush_scheduled_work();

	err = mmc_cache_ctrl(host, 0);
	if (err)
		goto out;

	mmc_bus_get(host);
	if (host->bus_ops && !host->bus_dead) {
		if (mmc_card_mmc(host->card) && !mmc_card_ignore_pon(host))
			mmc_poweroff_notify(host);

		if (host->bus_ops->suspend)
			err = host->bus_ops->suspend(host);

		if (err == -ENOSYS || !host->bus_ops->resume) {
			/*
			 * We simply "remove" the card in this case.
			 * It will be redetected on resume.  (Calling
			 * bus_ops->remove() with a claimed host can
			 * deadlock.)
			 */
			if (host->bus_ops->remove)
				host->bus_ops->remove(host);
			mmc_claim_host(host);
			mmc_detach_bus(host);
			mmc_power_off(host);
			mmc_release_host(host);
			host->pm_flags = 0;
			err = 0;
		}
	}
	mmc_bus_put(host);

	if (!err && !mmc_card_keep_power(host)) {
		if (host->card && !mmc_card_sd(host->card))
			mmc_power_off(host);
	}

out:
	return err;
}

EXPORT_SYMBOL(mmc_suspend_host);

/**
 *	mmc_resume_host - resume a previously suspended host
 *	@host: mmc host
 */
int mmc_resume_host(struct mmc_host *host)
{
	int err = 0;

	mmc_bus_get(host);
	if (mmc_bus_manual_resume(host)) {
		host->bus_resume_flags |= MMC_BUSRESUME_NEEDS_RESUME;
		mmc_bus_put(host);
		return 0;
	}

	if (host->bus_ops && !host->bus_dead) {
		if (!mmc_card_keep_power(host)) {
			mmc_power_up(host);
			mmc_select_voltage(host, host->ocr);
			/*
			 * Tell runtime PM core we just powered up the card,
			 * since it still believes the card is powered off.
			 * Note that currently runtime PM is only enabled
			 * for SDIO cards that are MMC_CAP_POWER_OFF_CARD
			 */
			if (mmc_card_sdio(host->card) &&
			    (host->caps & MMC_CAP_POWER_OFF_CARD)) {
				pm_runtime_disable(&host->card->dev);
				pm_runtime_set_active(&host->card->dev);
				pm_runtime_enable(&host->card->dev);
			}
		}
		BUG_ON(!host->bus_ops->resume);
		err = host->bus_ops->resume(host);
		if (err) {
			pr_warning("%s: error %d during resume "
					    "(card was removed?)\n",
					    mmc_hostname(host), err);
			err = 0;
		}
	}
	if (!host->card)
		mmc_power_off(host);
	host->pm_flags &= ~MMC_PM_KEEP_POWER;
	mmc_bus_put(host);

	return err;
}
EXPORT_SYMBOL(mmc_resume_host);

/* Do the card removal on suspend if card is assumed removeable
 * Do that in pm notifier while userspace isn't yet frozen, so we will be able
   to sync the card.
*/
int mmc_pm_notify(struct notifier_block *notify_block,
					unsigned long mode, void *unused)
{
	struct mmc_host *host = container_of(
		notify_block, struct mmc_host, pm_notify);
	unsigned long flags;


	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:

		spin_lock_irqsave(&host->lock, flags);
		if (mmc_bus_needs_resume(host)) {
			spin_unlock_irqrestore(&host->lock, flags);
			break;
		}
		host->rescan_disable = 1;
		host->power_notify_type = MMC_HOST_PW_NOTIFY_SHORT;
		spin_unlock_irqrestore(&host->lock, flags);
		if (cancel_delayed_work_sync(&host->detect))
			wake_unlock(&host->detect_wake_lock);

		if (!host->bus_ops || host->bus_ops->suspend)
			break;

		/* Calling bus_ops->remove() with a claimed host can deadlock */
		if (host->bus_ops->remove)
			host->bus_ops->remove(host);

		mmc_claim_host(host);
		mmc_detach_bus(host);
		mmc_power_off(host);
		mmc_release_host(host);
		host->pm_flags = 0;
		break;

	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:

		spin_lock_irqsave(&host->lock, flags);
		if (mmc_bus_manual_resume(host)) {
			spin_unlock_irqrestore(&host->lock, flags);
			break;
		}
		host->rescan_disable = 0;
		host->power_notify_type = MMC_HOST_PW_NOTIFY_LONG;
		spin_unlock_irqrestore(&host->lock, flags);
		mmc_detect_change(host, 0);
	}

	return 0;
}
#endif

#ifdef CONFIG_MMC_EMBEDDED_SDIO
void mmc_set_embedded_sdio_data(struct mmc_host *host,
				struct sdio_cis *cis,
				struct sdio_cccr *cccr,
				struct sdio_embedded_func *funcs,
				int num_funcs)
{
	host->embedded_sdio_data.cis = cis;
	host->embedded_sdio_data.cccr = cccr;
	host->embedded_sdio_data.funcs = funcs;
	host->embedded_sdio_data.num_funcs = num_funcs;
}

EXPORT_SYMBOL(mmc_set_embedded_sdio_data);
#endif

/*
 * Turn the bkops mode ON/OFF.
 */
int mmc_bkops_enable(struct mmc_host *host, u8 value)
{
	struct mmc_card *card = host->card;
	unsigned long flags;
	int err = 0;
	u8 ext_csd[512], bkops_en;

	if (!card)
		return err;

	mmc_claim_host(host);

	/* read ext_csd to get EXT_CSD_BKOPS_EN field value */
	err = mmc_send_ext_csd(card, ext_csd);
	if (err) {
		pr_err("%s: error %d sending ext_csd\n",
				mmc_hostname(card->host), err);
		goto bkops_out;
	}

	/* set value to put EXT_CSD_BKOPS_EN field */
	bkops_en = ext_csd[EXT_CSD_BKOPS_EN] & 0x1;
	value |= bkops_en;
	err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
			 EXT_CSD_BKOPS_EN, value,
			 card->ext_csd.generic_cmd6_time);
	if (!err) {
		spin_lock_irqsave(&card->bkops_lock, flags);
		card->bkops_enable = value;
		spin_unlock_irqrestore(&card->bkops_lock, flags);
	} else {
		pr_err("%s: bkops mode error %d\n", mmc_hostname(host), err);
	}

bkops_out:
	mmc_release_host(host);

	return err;
}
EXPORT_SYMBOL(mmc_bkops_enable);

/**
 * mmc_init_context_info() - init synchronization context
 * @host: mmc host
 *
 * Init struct context_info needed to implement asynchronous
 * request mechanism, used by mmc core, host driver and mmc requests
 * supplier.
 */
void mmc_init_context_info(struct mmc_host *host)
{
	spin_lock_init(&host->context_info.lock);
	host->context_info.is_new_req = false;
	host->context_info.is_done_rcv = false;
	host->context_info.is_waiting_last_req = false;
	init_waitqueue_head(&host->context_info.wait);
}

static int __init mmc_init(void)
{
	int ret;

	workqueue = alloc_ordered_workqueue("kmmcd", 0);
	if (!workqueue)
		return -ENOMEM;

	ret = mmc_register_bus();
	if (ret)
		goto destroy_workqueue;

	ret = mmc_register_host_class();
	if (ret)
		goto unregister_bus;

	ret = sdio_register_bus();
	if (ret)
		goto unregister_host_class;

	return 0;

unregister_host_class:
	mmc_unregister_host_class();
unregister_bus:
	mmc_unregister_bus();
destroy_workqueue:
	destroy_workqueue(workqueue);

	return ret;
}

static void __exit mmc_exit(void)
{
	sdio_unregister_bus();
	mmc_unregister_host_class();
	mmc_unregister_bus();
	destroy_workqueue(workqueue);
}

subsys_initcall(mmc_init);
module_exit(mmc_exit);

MODULE_LICENSE("GPL");
