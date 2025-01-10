/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Musumeci Salvatore
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#ifndef __TWAI_MOD_H__
    #define __TWAI_MOD_H__

    #include "soc/soc_caps.h"

#if SOC_TWAI_SUPPORTED

    #include "modmachine.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "freertos/event_groups.h"
    #include "driver/twai.h"
    #include "hal/twai_types.h"


    #include "py/obj.h"
    #include "py/runtime.h"
    #include "twai_message.h"

    #define TWAI_UNUSED(x)  ((void)x)

    #define TWAI_ERROR(type, msg, ...) mp_raise_msg_varg(&type, MP_ERROR_TEXT(msg), __VA_ARGS__)
    #define TWAI_INDEX_ERROR(msg, ...) TWAI_ERROR(mp_type_IndexError, msg, __VA_ARGS__)
    #define TWAI_VALUE_ERROR(msg, ...) TWAI_ERROR(mp_type_ValueError, msg, __VA_ARGS__)
    #define TWAI_OS_ERROR(msg, ...)    TWAI_ERROR(mp_type_OSError,    msg, __VA_ARGS__)


    typedef struct _mp_twai_registers_t {
        uint32_t bitrate;

        uint16_t brp : 9;
        uint8_t tseg1 : 5;
        uint8_t tseg2 : 4;
        uint8_t sjw : 3;
        uint16_t sample_point : 14;
        uint16_t err : 14;
        uint16_t total_tolerance : 14;
    } mp_twai_registers_t;


    typedef struct _mp_twai_lock_t {
        SemaphoreHandle_t handle;
        StaticSemaphore_t buffer;
    } mp_twai_lock_t;


    typedef struct _mp_twai_event_t {
        EventGroupHandle_t handle;
        StaticEventGroup_t buffer;
    } mp_twai_event_t;

    typedef struct _mp_twai_obj_t {
        mp_obj_base_t base;

        mp_twai_registers_t registers;
        twai_filter_config_t filter

        TaskHandle_t rx_task;
        mp_twai_lock_t init_lock;
        mp_twai_event_t task_exit;
        mp_twai_event_t filter_change;

        mp_obj_t alertcallback;

        mp_twai_lock_t bus_recovery_lock;

        mp_obj_t rxcallback;
        mp_twai_lock_t rx_lock;
        mp_obj_t *rx_buf;
        uint32_t rx_count;
        uint32_t rx_buf_len;

        mp_twai_lock_t tx_lock;
        mp_obj_t *tx_buf;
        uint32_t tx_count;
        uint32_t tx_buf_len;

        bool loopback : 1;

    } mp_twai_obj_t;


    typedef struct {
        twai_timing_config_t timing_config;
        twai_general_config_t general_config;
        mp_twai_obj_t *self;
        esp_err_t init_err;
        mp_rom_error_text_t init_err_msg;
    } mp_twai_task_arg_t;


    extern const mp_obj_type_t mp_twai_type_t;

#endif // SOC_TWAI_SUPPORTED

#endif // __TWAI_MOD_H__
