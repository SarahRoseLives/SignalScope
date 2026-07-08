/*
 * multimon_lib.c - Minimal library wrapper for multimon-ng FLEX decoder.
 */
#include "multimon_lib.h"
#include "multimon.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

// Global variables required by multimon-ng
int json_mode = 0;
int timestamp_mode = 0;

// Cosine lookup table (1024 entries) — not actually used by FLEX decoder,
// but required by multimon.h declarations.
const float costabf[0x400] = {1.0f};

// Forward declare FLEX demod param — defined in demod_flex.c
extern const struct demod_param demod_flex;

struct multimon_ctx {
    multimon_callback_t callback;
    void* user_data;
    int sample_rate;

    struct demod_state* flex_state;

    int overlap;
    float* audio_buffer;
    int audio_buffer_size;
    int buffer_pos;

    char line_buffer[4096];
    int line_pos;
};

static multimon_ctx_t* g_ctx = NULL;

// Stub functions required by multimon.h
void uart_init(struct demod_state *s) { (void)s; }
void uart_rxbit(struct demod_state *s, int bit) { (void)s; (void)bit; }
void clip_init(struct demod_state *s) { (void)s; }
void clip_rxbit(struct demod_state *s, int bit) { (void)s; (void)bit; }
void fms_init(struct demod_state *s) { (void)s; }
void fms_rxbit(struct demod_state *s, int bit) { (void)s; (void)bit; }
void pocsag_init(struct demod_state *s) { (void)s; }
void pocsag_rxbit(struct demod_state *s, int32_t bit) { (void)s; (void)bit; }
void pocsag_deinit(struct demod_state *s) { (void)s; }
void selcall_init(struct demod_state *s) { (void)s; }
void selcall_demod(struct demod_state *s, const float *buffer, int length,
                   const unsigned int *freq, const char *const name)
{ (void)s; (void)buffer; (void)length; (void)freq; (void)name; }
void selcall_deinit(struct demod_state *s) { (void)s; }
void addJsonTimestamp(cJSON *json) { (void)json; }
int xdisp_start(void) { return 0; }
int xdisp_update(int cnum, float *f) { (void)cnum; (void)f; return 0; }

void _verbprintf(int verb_level, const char *fmt, ...)
{
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (verb_level == 0 && g_ctx && g_ctx->callback) {
        int len = strlen(buffer);
        for (int i = 0; i < len; i++) {
            char c = buffer[i];
            if (c == '\n') {
                if (g_ctx->line_pos > 0) {
                    g_ctx->line_buffer[g_ctx->line_pos] = '\0';
                    const char* decoder_name = "UNKNOWN";
                    if (strncmp(g_ctx->line_buffer, "FLEX", 4) == 0)
                        decoder_name = "FLEX";
                    g_ctx->callback(decoder_name, g_ctx->line_buffer, g_ctx->user_data);
                    g_ctx->line_pos = 0;
                }
            } else if (g_ctx->line_pos < (int)sizeof(g_ctx->line_buffer) - 1) {
                g_ctx->line_buffer[g_ctx->line_pos++] = c;
            }
        }
    }
}

multimon_ctx_t* multimon_create(multimon_callback_t callback, void* user_data, int sample_rate)
{
    multimon_ctx_t* ctx = (multimon_ctx_t*)calloc(1, sizeof(multimon_ctx_t));
    if (!ctx) return NULL;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->sample_rate = sample_rate;
    ctx->overlap = 0;
    ctx->buffer_pos = 0;
    ctx->line_pos = 0;
    ctx->audio_buffer_size = 65536;
    ctx->audio_buffer = (float*)calloc(ctx->audio_buffer_size, sizeof(float));
    if (!ctx->audio_buffer) { free(ctx); return NULL; }
    g_ctx = ctx;
    return ctx;
}

int multimon_enable_decoder(multimon_ctx_t* ctx, multimon_decoder_t decoder)
{
    if (!ctx || decoder != MULTIMON_FLEX) return -1;

    struct demod_state* state = (struct demod_state*)calloc(1, sizeof(struct demod_state));
    if (!state) return -1;
    state->dem_par = &demod_flex;
    if (demod_flex.init) demod_flex.init(state);
    ctx->flex_state = state;
    if (demod_flex.overlap > (unsigned int)ctx->overlap)
        ctx->overlap = demod_flex.overlap;
    return 0;
}

void multimon_set_aprs_mode(multimon_ctx_t* ctx, int enable)
{
    (void)ctx; (void)enable;
}

int multimon_get_overlap(multimon_ctx_t* ctx)
{
    return ctx ? ctx->overlap : 0;
}

static int processSamples(multimon_ctx_t* ctx);

int multimon_process(multimon_ctx_t* ctx, const int16_t* samples, size_t count)
{
    if (!ctx || !samples || count == 0) return -1;
    size_t space = (size_t)(ctx->audio_buffer_size - ctx->buffer_pos);
    if (count > space) count = space;
    for (size_t i = 0; i < count; i++)
        ctx->audio_buffer[ctx->buffer_pos + i] = samples[i] / 32768.0f;
    ctx->buffer_pos += (int)count;
    return processSamples(ctx);
}

int multimon_process_float(multimon_ctx_t* ctx, const float* samples, size_t count)
{
    if (!ctx || !samples || count == 0) return -1;
    size_t space = (size_t)(ctx->audio_buffer_size - ctx->buffer_pos);
    if (count > space) count = space;
    memcpy(ctx->audio_buffer + ctx->buffer_pos, samples, count * sizeof(float));
    ctx->buffer_pos += (int)count;
    return processSamples(ctx);
}

static int processSamples(multimon_ctx_t* ctx)
{
    if (ctx->buffer_pos <= ctx->overlap) return 0;
    int process_len = ctx->buffer_pos - ctx->overlap;

    if (ctx->flex_state) {
        buffer_t buf;
        buf.fbuffer = ctx->audio_buffer;
        buf.sbuffer = NULL;
        demod_flex.demod(ctx->flex_state, buf, process_len);
    }

    if (ctx->overlap > 0 && ctx->buffer_pos > ctx->overlap) {
        memmove(ctx->audio_buffer, ctx->audio_buffer + process_len, ctx->overlap * sizeof(float));
        ctx->buffer_pos = ctx->overlap;
    } else {
        ctx->buffer_pos = 0;
    }
    return 0;
}

void multimon_destroy(multimon_ctx_t* ctx)
{
    if (!ctx) return;
    if (ctx->flex_state) {
        if (demod_flex.deinit) demod_flex.deinit(ctx->flex_state);
        free(ctx->flex_state);
    }
    if (ctx->audio_buffer) free(ctx->audio_buffer);
    if (g_ctx == ctx) g_ctx = NULL;
    free(ctx);
}
