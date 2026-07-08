/*
 * multimon_lib.h - Library wrapper for multimon-ng
 * Provides a simple API to run multimon-ng decoders
 */

#ifndef MULTIMON_LIB_H
#define MULTIMON_LIB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Multimon decoder handle */
typedef struct multimon_ctx multimon_ctx_t;

/* Decoded packet callback */
typedef void (*multimon_callback_t)(const char* decoder_name, const char* message, void* user_data);

/* Available decoder types */
typedef enum {
    MULTIMON_AFSK1200,
    MULTIMON_AFSK2400,
    MULTIMON_AFSK2400_2,
    MULTIMON_AFSK2400_3,
    MULTIMON_POCSAG512,
    MULTIMON_POCSAG1200,
    MULTIMON_POCSAG2400,
    MULTIMON_EAS,
    MULTIMON_UFSK1200,
    MULTIMON_CLIPFSK,
    MULTIMON_FMSFSK,
    MULTIMON_DTMF,
    MULTIMON_ZVEI1,
    MULTIMON_ZVEI2,
    MULTIMON_ZVEI3,
    MULTIMON_DZVEI,
    MULTIMON_PZVEI,
    MULTIMON_EEA,
    MULTIMON_EIA,
    MULTIMON_CCIR,
    MULTIMON_FLEX,
    MULTIMON_HAPN4800,
    MULTIMON_FSK9600,
    MULTIMON_MORSE,
    MULTIMON_X10,
    MULTIMON_SCOPE,
} multimon_decoder_t;

/**
 * Create multimon context
 * @param callback Callback function for decoded messages
 * @param user_data User data passed to callback
 * @param sample_rate Audio sample rate (must match decoder requirements, typically 22050)
 * @return Context handle or NULL on error
 */
multimon_ctx_t* multimon_create(multimon_callback_t callback, void* user_data, int sample_rate);

/**
 * Enable a decoder
 * @param ctx Context handle
 * @param decoder Decoder type to enable
 * @return 0 on success, -1 on error
 */
int multimon_enable_decoder(multimon_ctx_t* ctx, multimon_decoder_t decoder);

/**
 * Enable APRS mode for AFSK decoders
 * @param ctx Context handle
 * @param enable 1 to enable, 0 to disable
 */
void multimon_set_aprs_mode(multimon_ctx_t* ctx, int enable);

/**
 * Process audio samples through enabled decoders
 * @param ctx Context handle
 * @param samples Audio samples (int16_t)
 * @param count Number of samples
 * @return 0 on success, -1 on error
 */
int multimon_process(multimon_ctx_t* ctx, const int16_t* samples, size_t count);

/**
 * Process audio samples (float format)
 * @param ctx Context handle
 * @param samples Audio samples (float, -1.0 to 1.0)
 * @param count Number of samples
 * @return 0 on success, -1 on error
 */
int multimon_process_float(multimon_ctx_t* ctx, const float* samples, size_t count);

/**
 * Get required overlap for enabled decoders
 * @param ctx Context handle
 * @return Number of overlap samples needed
 */
int multimon_get_overlap(multimon_ctx_t* ctx);

/**
 * Destroy context and free resources
 * @param ctx Context handle
 */
void multimon_destroy(multimon_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* MULTIMON_LIB_H */
