/*
 * Copyright 2022 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "os/assert.h"
#include "os/stdlib.h"

#include "app_board.h"

#include "audio_pipeline.h"
#include "audio.h"
#include "hlog.h"
#include "hrpn_ctrl.h"
#include "codec_config.h"
#include "sai_clock_config.h"
#include "sai_drv.h"
#include "sai_config.h"

#define DEFAULT_PERIOD		8
#define DEFAULT_SAMPLE_RATE	48000
#define USE_TX_IRQ		1

static const int supported_period[] = {2, 4, 8, 16, 32};
static const uint32_t supported_rate[] = {44100, 48000, 88200, 96000, 176400, 192000};

struct pipeline_ctx {
	void (*event_send)(void *, uint8_t);
	void *event_data;

	struct sai_device dev[SAI_TX_MAX_INSTANCE];
	struct audio_pipeline *pipeline;
	sai_word_width_t bit_width;
	sai_sample_rate_t sample_rate;
	uint32_t chan_numbers;
	uint8_t period;

	struct {
		uint64_t callback;
		uint64_t run;
		uint64_t err;
	} stats;
};

void play_pipeline_stats(void *handle)
{
	struct pipeline_ctx *ctx = handle;

	log_info("callback: %llu, run: %llu, err: %llu\n", ctx->stats.callback, ctx->stats.run,
		ctx->stats.err);

	audio_pipeline_stats(ctx->pipeline);
}

static void rx_callback(uint8_t status, void *user_data)
{
	struct pipeline_ctx *ctx = (struct pipeline_ctx*)user_data;

#if USE_TX_IRQ
	sai_disable_irq(&ctx->dev[0], false, true);
#else
	sai_disable_irq(&ctx->dev[0], true, false);
#endif

	ctx->stats.callback++;

	ctx->event_send(ctx->event_data, status);
}

int play_pipeline_run(void *handle, struct event *e)
{
	struct pipeline_ctx *ctx = handle;
	int err;

	ctx->stats.run++;

	err = audio_pipeline_run(ctx->pipeline);
	if (err) {
		ctx->stats.err++;
		audio_pipeline_reset(ctx->pipeline);
		err = audio_pipeline_run(ctx->pipeline);
		os_assert(!err, "pipeline couldn't restart");
	}

#if USE_TX_IRQ
	sai_enable_irq(&ctx->dev[0], false, true);
#else
	sai_enable_irq(&ctx->dev[0], true, false);
#endif

	return err;
}

static void pll_adjust_disable(struct pipeline_ctx *ctx)
{
	struct hrpn_cmd_audio_element_pll cmd;

	/* need to disable PLL audio element */
	cmd.u.common.type = HRPN_CMD_TYPE_AUDIO_ELEMENT_PLL_DISABLE;
	cmd.u.common.pipeline.id = 0;
	cmd.u.common.element.type = AUDIO_ELEMENT_PLL;
	cmd.u.common.element.id = 0;

	audio_pipeline_ctrl((struct hrpn_cmd_audio_pipeline *)&cmd, sizeof(cmd), NULL);
}

static void pll_adjust_set_pll_id(struct pipeline_ctx *ctx, uint32_t pll_id)
{
	struct hrpn_cmd_audio_element_pll cmd;

	/* PLL element needs to know the sampling rate to determine the input PLL */
	cmd.u.common.type = HRPN_CMD_TYPE_AUDIO_ELEMENT_PLL_ID;
	cmd.u.common.pipeline.id = 0;
	cmd.u.common.element.type = AUDIO_ELEMENT_PLL;
	cmd.u.common.element.id = 0;
	cmd.pll_id = pll_id;

	audio_pipeline_ctrl((struct hrpn_cmd_audio_pipeline *)&cmd, sizeof(cmd), NULL);
}

static void sai_setup(struct pipeline_ctx *ctx)
{
	struct sai_cfg sai_config;
	bool pll_disable = true;
	int i;

	/* Configure each active SAI */
	for (i = 0; i < sai_active_list_nelems; i++) {
		uint32_t sai_clock_root, pll_id;
		int sai_id;
		enum codec_id cid;
		int32_t ret;

		sai_config.sai_base = sai_active_list[i].sai_base;
		sai_config.bit_width = ctx->bit_width;
		sai_config.sample_rate = ctx->sample_rate;
		sai_config.chan_numbers = ctx->chan_numbers;

		sai_id = get_sai_id(sai_active_list[i].sai_base);
		os_assert(sai_id, "SAI%d enabled but not supported in this platform!", i);

		sai_clock_root = get_sai_clock_root(sai_id - 1);
		sai_config.source_clock_hz = CLOCK_GetPllFreq(sai_active_list[i].audio_pll) / CLOCK_GetRootPreDivider(sai_clock_root) / CLOCK_GetRootPostDivider(sai_clock_root);

		sai_config.tx_sync_mode = sai_active_list[i].tx_sync_mode;
		sai_config.rx_sync_mode = sai_active_list[i].rx_sync_mode;
		sai_config.msel = sai_active_list[i].msel;

		if (i == 0) {
			/* First SAI instance used as IRQ source */
			sai_config.rx_callback = rx_callback;
			sai_config.rx_user_data = ctx;
			sai_config.working_mode = SAI_RX_IRQ_MODE;
		} else {
			sai_config.rx_callback = NULL;
			sai_config.rx_user_data = NULL;
			sai_config.working_mode = SAI_POLLING_MODE;
		}

		/* Configure attached codec */
		cid = sai_active_list[i].cid;
		ret = codec_setup(cid);
		if (ret != kStatus_Success) {
			if ((i == 0) && (sai_active_list[i].masterSlave == kSAI_Slave)) {
				/* First SAI in the list manages interrupts: it cannot be
				 * slave if no codec is connected
				 */
				log_info("No codec found on SAI%d, forcing master mode\n",
						get_sai_id(sai_active_list[i].sai_base));
				sai_active_list[i].masterSlave = kSAI_Master;
			}
		} else {
			codec_set_format(cid, sai_config.source_clock_hz, sai_config.sample_rate, ctx->bit_width);
		}

		sai_config.masterSlave = sai_active_list[i].masterSlave;

		if (sai_config.masterSlave == kSAI_Slave)
			pll_disable = false;

		/* Set FIFO water mark to be period size of all channels*/
#if USE_TX_IRQ
		sai_config.fifo_water_mark = ctx->period * ctx->chan_numbers - 1;
#else
		sai_config.fifo_water_mark = ctx->period * ctx->chan_numbers;
#endif

		sai_drv_setup(&ctx->dev[i], &sai_config);

		pll_id = sai_select_audio_pll_mux(sai_id, sai_config.sample_rate);
		pll_adjust_set_pll_id(ctx, pll_id);
	}

	if (pll_disable)
		pll_adjust_disable(ctx);
}

static void sai_close(struct pipeline_ctx *ctx)
{
	int i;

	/* Close each active SAI */
	for (i = 0; i < sai_active_list_nelems; i++) {
		sai_drv_exit(&ctx->dev[i]);
	}
}

void *play_pipeline_init(void *parameters)
{
	struct audio_config *cfg = parameters;
	struct play_pipeline_config *play_cfg = cfg->data;
	struct audio_pipeline_config *pipeline_cfg;
	struct pipeline_ctx *ctx;
	size_t period = DEFAULT_PERIOD;
	uint32_t rate = DEFAULT_SAMPLE_RATE;

	if (assign_nonzero_valid_val(period, cfg->period, supported_period) != 0) {
		log_err("Period %d frames is not supported\n", cfg->period);
		goto err;
	}

	if (assign_nonzero_valid_val(rate, cfg->rate, supported_rate) != 0) {
		log_err("Rate %d Hz is not supported\n", cfg->rate);
		goto err;
	}

	ctx = os_malloc(sizeof(struct pipeline_ctx) + sizeof(struct audio_pipeline_config));
	os_assert(ctx, "Audio pipeline failed with memory allocation error");
	memset(ctx, 0, sizeof(struct pipeline_ctx));

	pipeline_cfg = (struct audio_pipeline_config *)(ctx + 1);

	memcpy(pipeline_cfg, play_cfg->cfg, sizeof(struct audio_pipeline_config));

	/* override pipeline configuration */
	pipeline_cfg->sample_rate = rate;
	pipeline_cfg->period = period;

	ctx->pipeline = audio_pipeline_init(pipeline_cfg);
	if (!ctx->pipeline)
		goto err_init;

	ctx->sample_rate = rate;
	ctx->chan_numbers = DEMO_AUDIO_DATA_CHANNEL;
	ctx->bit_width = DEMO_AUDIO_BIT_WIDTH;
	ctx->period = period;
	ctx->event_send = cfg->event_send;
	ctx->event_data = cfg->event_data;

	sai_setup(ctx);

	log_info("Starting %s (Sample Rate: %d Hz, Period: %u frames)\n",
			pipeline_cfg->name, rate, (uint32_t)period);

	return ctx;

err_init:
	os_free(ctx);

err:
	return NULL;
}

void play_pipeline_exit(void *handle)
{
	struct pipeline_ctx *ctx = handle;
	int i;

	audio_pipeline_exit(ctx->pipeline);

	sai_close(ctx);

	for (i = 0; i < sai_active_list_nelems; i++)
		codec_close(sai_active_list[i].cid);

	os_free(ctx);

	log_info("\nEnd.\n");
}
