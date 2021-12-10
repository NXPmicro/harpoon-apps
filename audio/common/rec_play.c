/*
 * Copyright 2021 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "board.h"
#include "os/assert.h"
#include "os/stdlib.h"
#include "sai_drv.h"
#include "sai_codec_config.h"
#include "audio.h"

#define BUFFER_NUMBER		(2U)

struct sai_statistics {
	uint64_t rec_play_periods;
	uint64_t rx_fifo_errs;
	uint64_t tx_fifo_errs;
};

struct rec_play_ctx {
	void (*event_send)(void *, uint8_t);
	void *event_data;

	struct sai_device dev;
	struct sai_statistics stats;
	uint8_t *sai_buf;
	size_t period_bytes_per_chan;
	size_t buffer_size;
	uint32_t buf_index;
	sai_word_width_t bit_width;
	sai_sample_rate_t sample_rate;
	uint32_t chan_numbers;
};

void rec_play_stats(void *handle)
{
	struct rec_play_ctx *ctx = handle;

	os_printf("periods: %lld, rx error: %lld, tx error: %lld\r",
		ctx->stats.rec_play_periods, ctx->stats.rx_fifo_errs, ctx->stats.rx_fifo_errs);
}

static void rx_tx_callback(uint8_t status, void *user_data)
{
	struct rec_play_ctx *ctx = (struct rec_play_ctx*)user_data;

	os_assert(ctx, "userData is NULL in callback.");

	sai_disable_irq(&ctx->dev, true, false);

	ctx->event_send(ctx->event_data, status);
}

static void start_rec_play(struct rec_play_ctx *ctx)
{
	/* Reset FIFO for safe */
	reset_tx_fifo(&ctx->dev);
	reset_rx_fifo(&ctx->dev);

	/* Fill Tx FIFO with dummy data */
	memset(ctx->sai_buf, 0, ctx->buffer_size * BUFFER_NUMBER);

	/* Write two period into FIFO */
	sai_fifo_write(&ctx->dev, ctx->sai_buf,
			ctx->period_bytes_per_chan * 2);

	sai_enable_tx(&ctx->dev, false);
	sai_enable_rx(&ctx->dev, true);
}

int rec_play_run(void *parameters, struct event *e)
{
	struct rec_play_ctx *ctx = (struct rec_play_ctx*)parameters;
	struct sai_device *dev = &ctx->dev;
	uint8_t *sai_buffer = ctx->sai_buf;
	size_t period_bytes_per_chan = ctx->period_bytes_per_chan;
	size_t buffer_size = ctx->buffer_size;


	switch (e->type) {
	case EVENT_TYPE_START:
		/* Begin to record and playback */
		start_rec_play(ctx);
		break;

	case EVENT_TYPE_TX_RX:
		if (e->data == SAI_STATUS_NO_ERROR) {
			sai_fifo_read(dev, sai_buffer +
				ctx->buf_index * buffer_size,
				period_bytes_per_chan);

			sai_fifo_write(dev, sai_buffer +
				ctx->buf_index * buffer_size,
				period_bytes_per_chan);

			sai_enable_irq(dev, true, false);

			ctx->buf_index++;
			if (ctx->buf_index == BUFFER_NUMBER)
				ctx->buf_index = 0;

			ctx->stats.rec_play_periods++;
		} else {
			/* Restart the process in case of error */
			start_rec_play(ctx);

			if (e->data & SAI_STATUS_TX_FF_ERR)
				ctx->stats.tx_fifo_errs++;

			if (e->data & SAI_STATUS_RX_FF_ERR)
				ctx->stats.rx_fifo_errs++;
		}

		break;

	default:
		break;
	}

	return 0;
}

static void sai_setup(struct rec_play_ctx *ctx)
{
	struct sai_cfg sai_config;

	sai_config.sai_base = (void *)DEMO_SAI;
	sai_config.bit_width = ctx->bit_width;
	sai_config.sample_rate = ctx->sample_rate;
	sai_config.chan_numbers = ctx->chan_numbers;
	sai_config.source_clock_hz = DEMO_AUDIO_MASTER_CLOCK;
	sai_config.tx_sync_mode = DEMO_SAI_TX_SYNC_MODE;
	sai_config.rx_sync_mode = DEMO_SAI_RX_SYNC_MODE;
	sai_config.rx_callback = rx_tx_callback;
	sai_config.rx_user_data = ctx;
	sai_config.working_mode = SAI_CONTINUE_MODE;

	sai_drv_setup(&ctx->dev, &sai_config);
}

void *rec_play_init(void *parameters)
{
	struct audio_config *cfg = parameters;
	size_t frame_bytes_per_chan;
	size_t period_size = FSL_FEATURE_SAI_FIFO_COUNT / 2;
	size_t period_bytes_per_chan;
	size_t buffer_size;
	struct rec_play_ctx *ctx;

	frame_bytes_per_chan = DEMO_AUDIO_BIT_WIDTH / 8;
	period_bytes_per_chan = period_size * frame_bytes_per_chan;
	buffer_size = period_bytes_per_chan * DEMO_AUDIO_DATA_CHANNEL;

	ctx = os_malloc(sizeof(struct rec_play_ctx) + buffer_size * BUFFER_NUMBER);
	os_assert(ctx, "Record and playback failed with memory allocation error");
	memset(ctx, 0, sizeof(struct rec_play_ctx));
	ctx->sai_buf = (uint8_t *)(ctx + 1);

	ctx->sample_rate = DEMO_AUDIO_SAMPLE_RATE;
	ctx->chan_numbers = DEMO_AUDIO_DATA_CHANNEL;
	ctx->bit_width = DEMO_AUDIO_BIT_WIDTH;
	ctx->period_bytes_per_chan = period_bytes_per_chan;
	ctx->buffer_size = buffer_size;
	ctx->buf_index = 0;

	ctx->event_send = cfg->event_send;
	ctx->event_data = cfg->event_data;

	sai_setup(ctx);

	codec_setup();
	codec_set_format(DEMO_AUDIO_MASTER_CLOCK, ctx->sample_rate,
			ctx->bit_width);

	os_printf("Record and playback started (Sample Rate: %d Hz, Bit Width: %d bits)\r\n",
			ctx->sample_rate, ctx->bit_width);

	return ctx;
}

void rec_play_exit(void *parameters)
{
	struct rec_play_ctx *ctx = (struct rec_play_ctx*)parameters;

	sai_drv_exit(&ctx->dev);

	codec_close();

	os_free(ctx);

	os_printf("\r\nEnd.\r\n");
}
