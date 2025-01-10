/* The MIT License (MIT)
 *
 * Copyright (c) 2019 Musumeci Salvatore
 * Copyright (c) 2021 Ihor Nehrutsa
 * Copyright (c) 2022 Yuriy Makarov
 * Copyright (c) 2023 Viktor Vorobjov
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
 
 #include "soc/soc_caps.h"

#if SOC_TWAI_SUPPORTED
    #include <math.h>
    #include <string.h>
    
    #include "py/obj.h"
    #include "py/objarray.h"
    #include "py/binary.h"
    #include "py/runtime.h"
    #include "py/gc.h"
    #include "py/stackctrl.h"
    #include "py/builtin.h"
    #include "py/mphal.h"
    #include "py/mperrno.h"
    #include "mpconfigport.h"
    
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "freertos/semphr.h"
    #include "freertos/event_groups.h"
    #include "freertos/idf_additions.h"
    #include "rom/ets_sys.h"
    #include "esp_system.h"
    #include "esp_cpu.h"
    #include "esp_idf_version.h"
    #include "soc/soc.h"

#if CONFIG_IDF_TARGET_ESP32
    #include "soc/dport_reg.h"
#elif CONFIG_IDF_TARGET_ESP32C3
    #include "soc/system_reg.h"
    #include "soc/interrupt_reg.h"
    #include "soc/periph_defs.h"
    #include "soc/sensitive_reg.h"
#elif CONFIG_IDF_TARGET_ESP32S3
    #include "soc/system_reg.h"
    #include "soc/interrupt_core0_reg.h"
    #include "soc/interrupt_core1_reg.h"
    #include "soc/periph_defs.h"
    #include "soc/sensitive_reg.h"
#else
    #error "Unsupported target"
    #endif

#ifndef __ASSEMBLER__
    #include "soc/dport_access.h"
#endif

    #include "esp_err.h"
    #include "esp_log.h"
    #include "driver/twai.h"
    #include "hal/twai_types.h"
    #include "hal/twai_ll.h"  // twai_ll_check_brp_validation

    #include "esp_task.h"
    
    #include "twai_mod.h"
    #include "twai_message.h"
    
    
    #define TWAI_MIN(a, b) ((a) < (b) ? (a) : (b))
    #define TWAI_MIN3(a, b, c) (TWAI_MIN(TWAI_MIN(a,b), c))
    
    #define TWAI_DEBUG(...) ESP_LOGI("CAN", __VA_ARGS__)
    
    #define TWAI_EVENT_BIT_0 (1 << 0)
    #define TWAI_EVENT_INIT(x) x.handle = xEventGroupCreateStatic(&x.buffer)
    #define TWAI_EVENT_WAIT(x) xEventGroupWaitBits(x.handle, TWAI_EVENT_BIT_0, pdFALSE, pdTRUE, portMAX_DELAY)
    #define TWAI_EVENT_ISSET(x) (bool)(xEventGroupGetBits(x.handle) & TWAI_EVENT_BIT_0)
    #define TWAI_EVENT_SET(x) xEventGroupSetBits(x.handle, TWAI_EVENT_BIT_0)
    #define TWAI_EVENT_CLEAR(x) xEventGroupClearBits(x.handle, TWAI_EVENT_BIT_0)
    #define TWAI_EVENT_DELETE(x) \
        xEventGroupSetBits(x.handle, TWAI_EVENT_BIT_0); \
        vEventGroupDelete(x.handle)
    
    #define TWAI_LOCK_ACQUIRE(x, wait_ms) (bool)(pdTRUE == xSemaphoreTake(x.handle, wait_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS((uint16_t)wait_ms)))
    #define TWAI_LOCK_RELEASE(x) xSemaphoreGive(x.handle)
    #define TWAI_LOCK_DELETE(x) vSemaphoreDelete(x.handle);
    #define TWAI_LOCK_INIT(x) \
            x.handle = xSemaphoreCreateBinaryStatic(&x.buffer); \
            xSemaphoreGive(x.handle)
    
    
    #define CAN_MODE_SILENT_LOOPBACK (0x10)
        
    #define CAN_TASK_PRIORITY           (ESP_TASK_PRIO_MIN + 1)
    #define CAN_TASK_STACK_SIZE         (1024)


    static bool get_registers(uint32_t bitrate, uint8_t bus_len, uint16_t prop_delay, mp_twai_registers_t * registers)
    {
        uint32_t fsys = 80000000;
        if (TWAI_CLK_SRC_DEFAULT ==  SOC_MOD_CLK_XTAL) fsys = 40000000;
    
        uint16_t cycles_per_bit = (fsys / 1000) / (bitrate / 1000);
        uint16_t bit_time = 1000 / (bitrate / 1000);
        float prop_time = 2.0f * (0.005f * (float)bus_len + ((float)prop_delay / 1000.0f);
        float prop_fraction = prop_time / bit_time;
    
        registers->bitrate = 0;
        registers->total_tolerance = 0;
    
        uint16_t brp;
        int err;
        uint8_t prop_tq;
        uint8_t phase_seg2;
        uint8_t phase_seg1;
        uint8_t sjw_max;
        int df1;
        int df2;
        int osc_tol;
        int total_tol;
    
        uint8_t tseg1;
        uint8_t tseg2;
    
        for (uint8_t tq_bit=8;tq_bit<26;tq_bit++) {
            brp = cycles_per_bit / tq_bit;
    
            err = abs(tq_bit * brp - (int)cycles_per_bit) / (int)cycles_per_bit * 10000;
            prop_tq = (uint8_t)ceilf(prop_fraction * (float)tq_bit);
            phase_seg2 = (uint8_t)ceilf(((float)tq_bit - (1.0f + (float)prop_tq)) / 2.0f);
            phase_seg1 = tq_bit - (1 + prop_tq + phase_seg2);
            sjw_max = MIN3(phase_seg1, phase_seg2, 4);
    
            df1 = (int)(MIN(phase_seg1, phase_seg2) /
                                (2 * (13 * (int)tq_bit - (int)phase_seg2)) * 10000);
    
            df2 = (int)(sjw_max / (20 * tq_bit) * 10000);
            osc_tol = MIN(df1, df2);
    
            if (phase_seg1 <= 12 &&  phase_seg1 > 0 && phase_seg2 <= 12 &&
                                phase_seg2 > 0 && prop_tq <= 8 &&
                                1 + prop_tq + phase_seg2 + phase_seg1 == tq_bit) {
    
                total_tol = osc_tol - err;
                tseg1 = prop_tq + phase_seg1 - 1;
                tseg2 = phase_seg2 - 1;
    
                if (tseg1 >= 1 && tseg1 <= 16 && tseg2 >= 1 && tseg2 <= 8 &&
                                        total_tol > (int)registers->total_tolerance) {
    
                    registers->tseg1 = tseg1;
                    registers->tseg2 = tseg2;
    
                    registers->sjw = sjw_max - 1;
                    registers->sample_point = (uint16_t)((1 + prop_tq + phase_seg1) /
                                    (1 + prop_tq + phase_seg1 + phase_seg1) * 10000);
    
                    registers->brp = (uint8_t)fmodf(brp - 1.0f, powf(2.0f, 6.0f));
                    registers->bitrate = (uint32_t)(bitrate * (1 - (err / 10000)));
                    registers->err = (uint16_t)err;
                    registers->total_tolerance = (uint16_t)total_tol;
                }
            }
        }
    
        if (registers->bitrate == 0) return false;
        else return true;
    }
    
    
    
    static twai_status_info_t _twai_get_status() {
        twai_status_info_t status;
        check_esp_err(twai_get_status_info(&status));
        return status;
    }

    static void do_callback(mp_obj_t callback, mp_obj_t *args, size_t args_len)
    {
        volatile uint32_t sp = (uint32_t)esp_cpu_get_sp();
    
        void *old_state = mp_thread_get_state();
    
        mp_state_thread_t ts;
        mp_thread_set_state(&ts);
        mp_stack_set_top((void*)sp);
        mp_stack_set_limit(CONFIG_FREERTOS_IDLE_TASK_STACKSIZE - 1024);
        mp_locals_set(mp_state_ctx.thread.dict_locals);
        mp_globals_set(mp_state_ctx.thread.dict_globals);
    
        mp_sched_lock();
        gc_lock();
    
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_call_function_n_kw(callback, 0, args_len, args);
            nlr_pop();
        } else {
            ets_printf("Uncaught exception in TAWI IRQ callback handler!\n");
            mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        }
    
        gc_unlock();
        mp_sched_unlock();
    
        mp_thread_set_state(old_state);
    }
    
    
    
    // INTERNAL FUNCTION FreeRTOS IRQ task
    static void esp32_hw_can_irq_task(void *arg)
    {
        mp_twai_task_arg_t *task_arg = (mp_twai_task_arg_t *)arg;
        mp_twai_obj_t *self =task_arg->self;
    
        twai_timing_config_t timing_config = task_arg->timing_config;
        twai_general_config_t general_config = task_arg->general_config;
    
        task_arg->init_err = twai_driver_install(&general_config, &timing_config, &self->filter);
    
        if (task_arg->init_err != ESP_OK) {
            task_arg->init_err_msg = MP_ERROR_TEXT("%d(twai_driver_install)");
            TWAI_LOCK_RELEASE(self->init_lock);
            return;
        }
    
    
        // ESP_LOGI("CAN", "twai_driver_install");
        task_arg->init_err = twai_start();
        if (task_arg->init_err != ESP_OK) {
            task_arg->init_err_msg = MP_ERROR_TEXT("%d(twai_start)");
            TWAI_LOCK_RELEASE(self->init_lock);
            return;
        }
    
        TWAI_LOCK_RELEASE(self->init_lock);
    
        twai_reconfigure_alerts(TWAI_ALERT_ALL, NULL);
    
        TWAI_LOCK_RELEASE(self->init_lock);
    
        esp_err_t err;
        uint32_t alerts;
        bool exit_task = TWAI_EVENT_ISSET(self->task_exit);
        bool filter_change = !TWAI_EVENT_ISSET(self->filter_change);
    
        while (!exit_task) {
    
            if (filter_change) {
                twai_stop();
                twai_driver_uninstall();
                twai_driver_install(&general_config, &timing_config, &self->filter);
                twai_start();
                TWAI_EVENT_SET(self->filter_change);
            }
    
            alerts = 0;
            err = twai_read_alerts(&alerts, portMAX_DELAY);
            if (err != ESP_OK) {
                mp_printf(&mp_plat_print, "%d(twai_read_alerts)", err)
            } else {
                if (alerts & TWAI_ALERT_RX_DATA) {
                    alerts &= ~TWAI_ALERT_RX_DATA;
    
                    uint32_t msgs_to_rx = _esp32_hw_can_get_status().msgs_to_rx;
    
                    mp_obj_t args[msgs_to_rx] = { NULL };
                    twai_message_t rx_msg;
    
                    self->message_buf = malloc(sizeof(mp_obj_t) * rx_queue_size);
                    self->message_buf_len = rx_queue_size;
    
                    while (msgs_to_rx) {
                        TWAI_LOCK_ACQUIRE(self->rx_lock, -1);
                        uint32_t i = self->rx_count;
                        for (;i<msgs_to_rx;i++) {
                            if (i == self->rx_buf_len) break;
                            mp_twai_message_t *obj = MP_OBJ_TO_PTR(self->rx_buf[i]);
                            err = twai_receive(&obj->message, pdMS_TO_TICKS(10));
    
                            if (err != ESP_OK) mp_printf(&mp_plat_print, "%d(twai_read_alerts)", err)
                        }
    
                        if (self->rxcallback != mp_const_none) {
                            self->recv_count = 0;
                            do_callback(self->rxcallback, self->rx_buf, i)
                            msgs_to_rx -= i;
                        } else if (i != self->recv_count) {
                            msgs_to_rx -= i - self->rx_count;
                            self->rx_count = i;
                        }
                        TWAI_LOCK_RELEASE(self->rx_lock);
                    }
                }
    
                if (alerts & TWAI_ALERT_TX_FAILED || alerts & TWAI_ALERT_TX_SUCCESS) {
                    if (self->tx_count > 0) {
                        mp_obj_t obj = self->tx_buf[0];
    
                        mp_twai_message_t *mess = MP_OBJ_TO_PTR(self->tx_buf[0]);
                        TWAI_LOCK_ACQUIRE(self->tx_lock, -1);
                        for (uint8_t i=1;i<self->tx_count;i++) {
                            self->tx_buf[i - 1] = self->tx_buf[i];
                        }
                        self->tx_count--;
    
                        self->tx_buf[self->tx_count] = NULL;
                        TWAI_LOCK_RELEASE(self->tx_lock);
                        if (mess->tx_callback != mp_const_none) {
                            mp_obj_t args[2] = {
                                mp_obj_new_int(alerts & TWAI_ALERT_TX_FAILED ?
                                        TWAI_ALERT_TX_FAILED : TWAI_ALERT_TX_SUCCESS),
                                obj
                            };
    
                            do_callback(mess->tx_callback, args 2)
                            alerts &= ~(TWAI_ALERT_TX_FAILED | TWAI_ALERT_TX_SUCCESS);
                        }
                    }
                }
    
                if (alerts & TWAI_ALERT_BUS_RECOVERED) {
                    TWAI_LOCK_RELEASE(self->bus_recovery_lock);
                }
    
                if (alerts != 0 && self->alertcallback != mp_const_none) {
                    mp_obj_t args[1] = { mp_obj_new_int_from_uint(alerts), };
                    do_callback(self->alertcallback, args, 1)
                }
            }
    
            taskYIELD();
            exit_task = TWAI_EVENT_ISSET(self->task_exit);
            filter_change = !TWAI_EVENT_ISSET(self->filter_change);
        }
    
        TWAI_LOCK_RELEASE(self->bus_recovery_lock);
        TWAI_LOCK_DELETE(self->bus_recovery_lock);
        TWAI_LOCK_DELETE(self->rx_lock);
        TWAI_LOCK_DELETE(self->tx_lock);
        TWAI_EVENT_DELETE(self->task_exit);
    
        TWAI_EVENT_WAIT(self->task_exit);
        TWAI_EVENT_ISSET(self->task_exit);
        TWAI_LOCK_RELEASE(self->init_lock);
    
        vTaskDelete(self->rx_task);
        self->rx_task = NULL;
    }
    
    
    #define TWAI_CHECK(cond, ret_val) ({                                        \
                if (!(cond)) {                                                  \
                    return (ret_val);                                           \
                }                                                               \
    })
    
    
    
    
    static mp_obj_t esp32_hw_can_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
    {
        enum { ARG_tx_pin, ARG_rx_pin, ARG_bitrate, ARG_mode, ARG_brp, ARG_tseg1, ARG_tseg2, ARG_sjw,
               ARG_tx_queue_size, ARG_rx_queue_size, ARG_bus_off_pin, ARG_bus_length, ARG_prop_delay };
    
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_tx_pin,        MP_ARG_INT  | MP_ARG_REQUIRED },
            { MP_QSTR_rx_pin,        MP_ARG_INT  | MP_ARG_REQUIRED },
            { MP_QSTR_bitrate,       MP_ARG_INT  | MP_ARG_REQUIRED },
            { MP_QSTR_mode,          MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = TWAI_MODE_NORMAL } },
            { MP_QSTR_brp,           MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 0   } },
            { MP_QSTR_tseg1,         MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 0   } },
            { MP_QSTR_tseg2,         MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 0   } },
            { MP_QSTR_sjw,           MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 0   } },
            { MP_QSTR_tx_queue_size, MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 1   } },
            { MP_QSTR_rx_queue_size, MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 1   } },
            { MP_QSTR_bus_off_pin,   MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = (int)TWAI_IO_UNUSED } },
            { MP_QSTR_bus_length,    MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 0   } },
            { MP_QSTR_prop_delay,    MP_ARG_INT  | MP_ARG_KW_ONLY, { .u_int = 100 } },
        };
    
        // parse args
        mp_arg_val_t args[MP_ARRAY_SIZE(make_new_args)];
        mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
                MP_ARRAY_SIZE(make_new_args), make_new_args, args);
        
        // create new object
        mp_twai_obj_t *self = m_new_obj(mp_twai_obj_t);
        self->base.type = &mp_twai_type_t;
        
        self->rxcallback = mp_const_none;
        self->alertcallback = mp_const_none;
    
        uint32_t bitrate = (uint32_t)args[ARG_bitrate].u_int;
        uint16_t brp = (uint16_t)args[ARG_brp].u_int;
        uint8_t tseg1 = (uint8_t)args[ARG_tseg1].u_int;
        uint8_t tseg2 = (uint8_t)args[ARG_tseg2].u_int;
        uint8_t sjw = (uint8_t)args[ARG_sjw].u_int;
        uint8_t bus_length = (uint8_t)args[ARG_bus_length].u_int;
        uint16_t prop_delay = (uint16_t)args[ARG_prop_delay].u_int;
    
        if (sjw > 4) TWAI_VALUE_ERROR("sjw maximum is 4");
        if (tseg1 > 16) TWAI_VALUE_ERROR("tseg1 maximum is 16");
        if (tseg2 > 8) TWAI_VALUE_ERROR("tseg2 maximum is 8");
    
        if (brp > TWAI_BRP_MAX) TWAI_VALUE_ERROR("brp maximum is %d", TWAI_BRP_MAX);
        if (brp > 0) {
            if (brp < TWAI_BRP_MIN) TWAI_VALUE_ERROR("brp minimum is %d", TWAI_BRP_MIN);
            if (!twai_ll_check_brp_validation((uint32_t)brp)) TWAI_VALUE_ERROR("incorrect brp value %u", brp);
        }
    
        if (brp == 0 || tseg1 == 0 || tseg2 == 0 || sjw == 0) {
            if (bus_length == 0) TWAI_VALUE_ERROR("bus_length needs to be > 0")
            if (prop_delay == 0) TWAI_VALUE_ERROR("prop_delay needs to be > 0")
            if (!get_registers(bitrate, bus_len, prop_delay, &self->registers)) TWAI_VALUE_ERROR("unable to set target bitrate (%u)", bitrate)
        } else {
            self->registers.bitrate = bitrate;
            self->registers.brp = brp;
            self->registers.tseg1 = tseg1;
            self->registers.tseg2 = tseg2;
            self->registers.sjw = sjw;
        }
    
        twai_timing_config_t timing_config = {
            .clk_src=TWAI_CLK_SRC_DEFAULT,
            .quanta_resolution_hz=0,
            .brp=(uint32_t)self->registers.brp,
            .tseg_1=self->registers.tseg1,
            .tseg_2=self->registers.tseg2,
            .sjw=self->registers.sjw,
            .triple_sampling=false
        };
    
        uint32_t tx_queue_size = (uint32_t)args[ARG_tx_queue_size].u_int;
        uint32_t rx_queue_size = (uint32_t)args[ARG_rx_queue_size].u_int;
    
        if (tx_queue_size == 0) TWAI_VALUE_ERROR("tx_queue_size must be > 0")
        if (rx_queue_size == 0) TWAI_VALUE_ERROR("rx_queue_size must be > 0")
    
        twai_general_config_t general_config = {
            .controller_id=0,
            .mode=(twai_mode_t)args[ARG_mode].u_int & 0x0F,
            .tx_io=(gpio_num_t)args[ARG_tx_io].u_int,
            .rx_io=(gpio_num_t)args[ARG_rx_io].u_int,
            .clkout_io=TWAI_IO_UNUSED,
            .bus_off_io=(gpio_num_t)args[ARG_bus_off_pin].u_int,
            .tx_queue_len=tx_queue_size,
            .rx_queue_len=rx_queue_size,
            .alerts_enabled=TWAI_ALERT_ALL,
            .clkout_divider=0,
            .intr_flags=0
        };
    
        self->tx_buf = malloc(sizeof(mp_obj_t) * rx_queue_size);
        self->tx_buf_len = tx_queue_size;
    
        self->rx_buf = malloc(sizeof(mp_obj_t) * rx_queue_size);
        self->rx_buf_len = rx_queue_size;
    
        for (uint32_t i=0;i<rx_queue_size;i++) {
            mp_twai_message_t *message = m_new_obj(mp_twai_message_t);
            message->base.type = &mp_twai_message_type_t;
            self->message_buf[i] = MP_OBJ_FROM_PTR(self);
        }
    
        self->filter = (twai_filter_config_t) TWAI_FILTER_CONFIG_ACCEPT_ALL();
        self->loopback = ((args[ARG_mode].u_int & CAN_MODE_SILENT_LOOPBACK) > 0);
    
        TWAI_DEBUG("TIMING");
        TWAI_DEBUG("brp=%lu", timing_config.brp);
        TWAI_DEBUG("tseg_1=%u", timing_config.tseg_1);
        TWAI_DEBUG("tseg_2=%u", timing_config.tseg_2);
        TWAI_DEBUG("sjw=%u", timing_config.sjw);
        TWAI_DEBUG("triple_sampling=%u", (uint8_t)timing_config.triple_sampling);
    
        TWAI_DEBUG("target bitrate %u", bitrate);
        TWAI_DEBUG("actual bitrate %u", self->registers.bitrate);
        TWAI_DEBUG("mode %d", general_config.mode);
    
        mp_twai_task_arg_t task_arg = {
            general_config = general_config,
            timing_config = timing_config,
            self=self
        };
    
        TWAI_LOCK_INIT(self->init_lock);
        TWAI_EVENT_INIT(self->task_exit);
        TWAI_EVENT_INIT(self->filter_change);
        TWAI_EVENT_SET(self->filter_change);
    
        TWAI_LOCK_INIT(self->rx_lock);
        TWAI_LOCK_INIT(self->tx_lock);
        TWAI_LOCK_INIT(self->bus_recovery_lock);
        TWAI_LOCK_ACQUIRE(self->bus_recovery_lock, -1);
        TWAI_LOCK_ACQUIRE(self->init_lock, -1);
    
        if (xTaskCreatePinnedToCore(esp32_hw_can_irq_task, "can_irq_task", CAN_TASK_STACK_SIZE, &task_arg, CAN_TASK_PRIORITY, &self->rx_task, 0) != pdPASS) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("failed to create twai task handler"));
        }
    
        TWAI_LOCK_ACQUIRE(self->init_lock, -1);
        if (task_arg.init_err != ESP_OK) TWAI_OS_ERROR(task_arg.init_err_msg, task_arg.init_err);
    
        return MP_OBJ_FROM_PTR(self);
    }
    
    
    
    
    
    // deinit()
    static mp_obj_t twai_deinit(const mp_obj_t self_in)
    {
        const mp_twai_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
        TWAI_LOCK_ACQUIRE(self->init_lock, -1);
        TWAI_EVENT_SET(self->task_exit);
        TWAI_LOCK_ACQUIRE(self->init_lock, -1);
        TWAI_LOCK_RELEASE(self->init_lock);
        TWAI_LOCK_DELETE(self->init_lock);
    
        esp_err_t err = twai_stop();
        if (err != ESP_OK) TWAI_OS_ERROR('twai_stop (%d)', err);
        err = twai_driver_uninstall();
        if (err != ESP_OK) TWAI_OS_ERROR('twai_driver_uninstall (%d)', err);
    
        return mp_const_none;
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_deinit_obj, twai_deinit);
    
    
    // Force a software restart of the controller, to allow transmission after a bus error
    static mp_obj_t twai_restart(mp_obj_t self_in)
    {
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
        esp_err_t err = twai_initiate_recovery()
    
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            TWAI_OS_ERROR('twai_initiate_recovery (%d)', err);
        } else if (err != ESP_ERR_INVALID_STATE) {
            if (TWAI_LOCK_ACQUIRE(self->bus_recovery_lock, 3000)) {
                err = twai_start();
                if (err != ESP_OK) {
                    TWAI_OS_ERROR('twai_start (%d)', err);
                }
            } else {
                mp_raise_OSError(MP_EIO);
            }
        }
        return mp_const_none;
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_restart_obj, twai_restart);
    
    
    // Get the state of the controller
    static mp_obj_t twai_state(mp_obj_t self_in)
    {
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(self_in);
        state = _esp32_hw_can_get_status().state;
    
        return mp_obj_new_int(state);
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_state_obj, twai_state);
    
    
    static mp_obj_t twai_info(mp_obj_t self_in)
    {
        TWAI_UNUSED(self_in);
    
        twai_status_info_t status = _twai_get_status();
        mp_obj_t dict = mp_obj_new_dict(0);
        #define dict_key(key) mp_obj_new_str(#key, strlen(#key))
        #define dict_value(key) MP_OBJ_NEW_SMALL_INT(status.key)
        #define dict_store(key) mp_obj_dict_store(dict, dict_key(key), dict_value(key));
        dict_store(state);
        dict_store(msgs_to_tx);
        dict_store(msgs_to_rx);
        dict_store(tx_error_counter);
        dict_store(rx_error_counter);
        dict_store(tx_failed_count);
        dict_store(rx_missed_count);
        dict_store(arb_lost_count);
        dict_store(bus_error_count);
        return dict;
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_info_obj, twai_info);
    
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_alert_obj, twai_alert);
    
    
    // any() - return `True` if any message waiting, else `False`
    static mp_obj_t twai_messages_waiting(mp_obj_t self_in)
    {
        twai_status_info_t status = _twai_get_status();
        return mp_obj_new_int_from_uint(status.msgs_to_rx);
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_messages_waiting_obj, twai_messages_waiting);
    
    
    // send([data], id, *, timeout=0, rtr=false, extframe=false)
    static mp_obj_t twai_transmit(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
    {
        enum { ARG_self, ARG_message, ARG_timeout, };
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_self,     MP_ARG_REQUIRED | MP_ARG_OBJ },
            { MP_QSTR_message,  MP_ARG_REQUIRED | MP_ARG_OBJ },
            { MP_QSTR_timeout,  MP_ARG_KW_ONLY  | MP_ARG_INT,  { .u_int = -1 } }
        };
    
        // parse args
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args,
                                   MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
        mp_obj_t tx_obj = args[ARG_message].u_obj;
        mp_twai_message_t *self = MP_OBJ_TO_PTR(tx_obj);
    
        if (self->loopback) {
            tx_msg.flags += TWAI_MSG_FLAG_SELF;
        }
    
        int timeout_ms = (int)args[ARG_timeout].u_int;
        esp_err_t err;
    
        TWAI_LOCK_ACQUIRE(self->tx_lock, -1);
        if (self->tx_buf_len > 0 && self->tx_count == self->tx_buf_len) {
            TWAI_LOCK_RELEASE(self->tx_lock);
    
            if (timeout_ms > 0) {
                TWAI_LOCK_ACQUIRE(self->init_lock, timeout_ms);
                TWAI_LOCK_ACQUIRE(self->tx_lock, -1);
    
                if (self->tx_count == self->tx_buf_len) {
                    TWAI_LOCK_RELEASE(self->tx_lock);
                    return mp_const_false;
                }
            } else {
                    return mp_const_false;
            }
        }
    
        if (self->tx_buf_len > 0) {
            self->tx_buf[self->tx_count] = tx_obj;
            self->tx_count++;
        }
    
        TWAI_LOCK_RELEASE(self->tx_lock);
        if (timeout_ms == -1) {
            err = twai_transmit(&tx_msg, portMAX_DELAY);
        } else {
            err = twai_transmit(&tx_msg, pdMS_TO_TICKS(timeout_ms));
        }
    
        if (err != ESP_OK) {
            return mp_const_false;
        } else {
            return mp_const_true;
        }
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_KW(twai_transmit_obj, 2, twai_transmit);
    
    
    static mp_obj_t twai_receive(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
    {
        enum { ARG_self, ARG_count };
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_self,  MP_ARG_OBJ,},
            { MP_QSTR_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        };
    
        // parse args
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args,
                                   MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
        uint32_t count = (uint32_t)args[ARG_count].u_obj;
        mp_obj_t res = mp_obj_new_list(0, NULL);
    
        TWAI_LOCK_ACQUIRE(self->rx_lock, -1);
        if (self->rx_buf_len > 0) {
            if (count > self->rx_count) count = self->rx_count;
    
            for (uint32_t i=0;i<count;i++) {
                mp_obj_t mess_copy = twai_message_copy(self->rx_buf[i])
                mp_obj_list_append(res, mess_copy);
            }
    
            self->rx_count -= count;
        } else {
            uint32_t waiting_count = (uint32_t)mp_obj_get_int_truncated(twai_messages_waiting(args[ARG_self].u_obj));
    
            if (count > waiting_count) count = waiting_count;
    
            while (count) {
                mp_twai_message_t *obj = m_new_obj(mp_twai_message_t);
                obj->base.type = &mp_twai_message_type_t;
    
                err = twai_receive(&obj->message, pdMS_TO_TICKS(500))
                if (err == ESP_ERR_TIMEOUT) {
                    TWAI_LOCK_RELEASE(self->rx_lock);
                    return res;
                } else if (err != ESP_OK) {
                    TWAI_LOCK_RELEASE(self->rx_lock);
                    TWAI_OS_ERROR("twai_receive", err);
                    return res;
                }
    
                count -= 1;
    
                mp_obj_list_append(res, MP_OBJ_FROM_PTR(obj));
            }
        }
    
        TWAI_LOCK_RELEASE(self->rx_lock);
        return res;
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_hw_can_recv_obj, 1, esp32_hw_can_recv);
    
    
    // Clear filters setting
    static mp_obj_t twai_clear_filter(mp_obj_t self_in)
    {
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
        // Defaults from TWAI_FILTER_CONFIG_ACCEPT_ALL
        self->filter = (twai_filter_config_t) TWAI_FILTER_CONFIG_ACCEPT_ALL();
        TWAI_EVENT_CLEAR(self->filter_change);
        TWAI_EVENT_WAIT(self->filter_change);
        return mp_const_none;
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_clear_filter_obj, twai_clear_filter);
    
    
    static mp_obj_t twai_set_filter(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
    {
        enum { ARG_self, ARG_ids1, ARG_ids2, ARG_rtr1, ARG_rtr2, ARG_extframe };
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_self,     MP_ARG_OBJ  | MP_ARG_REQUIRED },
            { MP_QSTR_ids1,     MP_ARG_OBJ  | MP_ARG_REQUIRED },
            { MP_QSTR_ids2,     MP_ARG_OBJ, { .u_obj = mp_const_none } },
            { MP_QSTR_rtr1,     MP_ARG_BOOL | MP_ARG_KW_ONLY, { .u_bool = false } },
            { MP_QSTR_rtr2,     MP_ARG_BOOL | MP_ARG_KW_ONLY, { .u_bool = false } },
            { MP_QSTR_extframe, MP_ARG_BOOL | MP_ARG_KW_ONLY, { .u_bool = false } },
        };
    
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    
        bool rtr1 = (bool)args[ARG_rtr1].u_bool;
        bool rtr2 = (bool)args[ARG_rtr2].u_bool;
        bool extframe = (bool)args[ARG_extframe].u_bool;
    
        mp_obj_list_t *in_ids1 = MP_OBJ_TO_PTR(args[ARG_ids1].u_obj)
        uint32_t ids1_mask = 0;
    
        uint32_t id1_mask = 0;
        uint32_t id = 0;
    
        bool dual_mode = false;
    
        if (args[ARG_ids2].u_obj != mp_const_none) dual_mode = true;
    
        for (size_t i=0;i<in_ids1->len;i++) {
            id = mp_obj_get_int_truncated(in_ids1->items[i]);
    
            if (extframe && dual_mode) id = (id & 0xFFFFFFF) >> 13);
    
            id1_mask ^= id;
        }
    
        uint32_t id1_code = id & ~id1_mask;
    
        uint32_t code = 0;
        uint32_t mask = 0;
    
        if (dual_mode) {
            mp_obj_list_t *in_ids2 = MP_OBJ_TO_PTR(args[ARG_ids2].u_obj)
            uint32_t id2_mask = 0;
            uint32_t id = 0;
    
            for (size_t j=0;j<in_ids2->len;j++) {
                id = mp_obj_get_int_truncated(in_ids2->items[j]);
    
                if (extframe) id = (id & 0xFFFFFFF) >> 13);
    
                id2_mask ^= id;
            }
    
            uint32_t id2_code = id & ~id2_mask;
    
            if (extframe) {
                code = id2_code | (id1_code >> 16);
                mask = id2_mask | (id1_mask >> 16);
            } else {
                code = ((uint32_t)rtr2 >> 4) | (id2_code >> 5) | ((uint32_t)rtr1 >> 20) | (id1_code >> 21);
                mask = (id2_code >> 5) | (id1_mask >> 21);
            }
        } else {
            if (extframe) {
                code = ((uint32_t)rtr1 >> 2) | (id1_code >> 3);
                mask = id1_mask >> 3;
            } else {
                code = ((uint32_t)rtr1 >> 20) | (id1_code >> 21);
                mask = id1_mask >> 21;
            }
        }
    
        self->filter.single_filter = dual_mode;
        self->filter.acceptance_mask = mask;
        self->filter.acceptance_code = code;
    
        TWAI_EVENT_CLEAR(self->filter_change);
        TWAI_EVENT_WAIT(self->filter_change);
        return mp_const_none;
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_KW(twai_set_filter_obj, 2, twai_set_filter);
    
    
    // rxcallback(callable)
    static mp_obj_t twai_set_callbacks(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
    {
        enum { ARG_self, ARG_rx_cb, ARG_alert_cb };
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_self,     MP_ARG_OBJ  | MP_ARG_REQUIRED },
            { MP_QSTR_rx_cb,    MP_ARG_OBJ, { .u_obj = NULL } },
            { MP_QSTR_alert_cb, MP_ARG_OBJ, { .u_obj = NULL } }
        };
    
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
        
        if (args[ARG_rx_cb].u_obj != NULL) self->rxcallback = args[ARG_rx_cb].u_obj;
        if (args[ARG_alert_cb].u_obj != NULL) self->alertcallback = args[ARG_alert_cb].u_obj;
    
        return mp_const_none;
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_KW(twai_set_callbacks_obj, 1, twai_set_callbacks);
    
    
    // Clear TX Queue
    static mp_obj_t twai_clear_tx_queue(mp_obj_t self_in)
    {
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(self_in);
            
        bool res = (bool)(twai_clear_transmit_queue() == ESP_OK);
        
        if (res && self->tx_buf_len > 0) {
            TWAI_LOCK_ACQUIRE(self->tx_lock, -1);
            
            for (uint32_t i=0;i<self->tx_buf_len;i++) self->tx_buf[i] = NULL;
            self->tx_count = 0;                                              
            TWAI_LOCK_RELEASE(self->tx_lock)
        }
        
        return mp_obj_new_bool(res);
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_clear_tx_queue_obj, twai_clear_tx_queue);
    
    
    // Clear RX Queue
    static mp_obj_t twai_clear_rx_queue(mp_obj_t self_in)
    {
        mp_twai_obj_t *self = MP_OBJ_TO_PTR(self_in);
            
        bool res = (bool)(twai_clear_receive_queue() == ESP_OK);
        
        if (res && self->rx_buf_len > 0) {
            TWAI_LOCK_ACQUIRE(self->tx_lock, -1);
            self->rx_count = 0;                                              
            TWAI_LOCK_RELEASE(self->tx_lock)
        }
            
        return mp_obj_new_bool(res);
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(twai_clear_rx_queue_obj, twai_clear_rx_queue);
    
    
    static const mp_rom_map_elem_t twai_locals_dict_table[] = {
        { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_TWAI) },
    
        { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&twai_deinit_obj) },
        { MP_ROM_QSTR(MP_QSTR_restart), MP_ROM_PTR(&twai_restart_obj) },
        { MP_ROM_QSTR(MP_QSTR_state), MP_ROM_PTR(&twai_state_obj) },
        { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&twai_info_obj) },
        { MP_ROM_QSTR(MP_QSTR_messages_waiting), MP_ROM_PTR(&twai_messages_waiting_obj) },
        { MP_ROM_QSTR(MP_QSTR_transmit), MP_ROM_PTR(&twai_transmit_obj) },
        { MP_ROM_QSTR(MP_QSTR_receive), MP_ROM_PTR(&twai_receive_obj) },
        { MP_ROM_QSTR(MP_QSTR_set_filter), MP_ROM_PTR(&twai_set_filter_obj) },
        { MP_ROM_QSTR(MP_QSTR_clear_filter), MP_ROM_PTR(&twai_clear_filter_obj) },
        { MP_ROM_QSTR(MP_QSTR_set_callnacks), MP_ROM_PTR(&twai_set_callbacks_obj) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_clear_tx_queue), MP_ROM_PTR(&twai_clear_tx_queue_obj) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_clear_rx_queue), MP_ROM_PTR(&twai_clear_rx_queue_obj) },
    };
    
    static MP_DEFINE_CONST_DICT(twai_locals_dict, twai_locals_dict_table);
    
    
    // Python object definition
    MP_DEFINE_CONST_OBJ_TYPE(
        mp_twai_type_t,
        MP_QSTR_TWAI,
        MP_TYPE_FLAG_NONE,
        make_new, twai_make_new,
        locals_dict, (mp_obj_dict_t *)&twai_locals_dict
    );
    
    
    static const mp_rom_map_elem_t twai_module_globals_table[] = {
        { MP_ROM_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_twai)       },
        { MP_ROM_QSTR(MP_QSTR_TWAI),     MP_ROM_PTR(&mp_twai_type_t)         },
        { MP_ROM_QSTR(MP_QSTR_Message),  MP_ROM_PTR(&mp_twai_message_type_t) },
    
        { MP_ROM_QSTR(MP_QSTR_MODE_NORMAL),   MP_ROM_INT(TWAI_MODE_NORMAL)                            },
        { MP_ROM_QSTR(MP_QSTR_MODE_LOOPBACK), MP_ROM_INT(TWAI_MODE_NORMAL | CAN_MODE_SILENT_LOOPBACK) },
        { MP_ROM_QSTR(MP_QSTR_MODE_SILENT),   MP_ROM_INT(TWAI_MODE_NO_ACK)                            },
        { MP_ROM_QSTR(MP_QSTR_MODE_LISTEN),   MP_ROM_INT(TWAI_MODE_LISTEN_ONLY)                       },
    
        { MP_ROM_QSTR(MP_QSTR_STATE_STOPPED),    MP_ROM_INT(TWAI_STATE_STOPPED)    },
        { MP_ROM_QSTR(MP_QSTR_STATE_RUNNING),    MP_ROM_INT(TWAI_STATE_RUNNING)    },
        { MP_ROM_QSTR(MP_QSTR_STATE_OFF),        MP_ROM_INT(TWAI_STATE_BUS_OFF)    },
        { MP_ROM_QSTR(MP_QSTR_STATE_RECOVERING), MP_ROM_INT(TWAI_STATE_RECOVERING) },
    
        { MP_ROM_QSTR(MP_QSTR_ALERT_TX_IDLE),              MP_ROM_INT(TWAI_ALERT_TX_IDLE)              },
        { MP_ROM_QSTR(MP_QSTR_ALERT_TX_SUCCESS),           MP_ROM_INT(TWAI_ALERT_TX_SUCCESS)           },
        { MP_ROM_QSTR(MP_QSTR_ALERT_BELOW_ERR_WARN),       MP_ROM_INT(TWAI_ALERT_BELOW_ERR_WARN)       },
        { MP_ROM_QSTR(MP_QSTR_ALERT_ERR_ACTIVE),           MP_ROM_INT(TWAI_ALERT_ERR_ACTIVE)           },
        { MP_ROM_QSTR(MP_QSTR_ALERT_RECOVERY_IN_PROGRESS), MP_ROM_INT(TWAI_ALERT_RECOVERY_IN_PROGRESS) },
        { MP_ROM_QSTR(MP_QSTR_ALERT_BUS_RECOVERED),        MP_ROM_INT(TWAI_ALERT_BUS_RECOVERED)        },
        { MP_ROM_QSTR(MP_QSTR_ALERT_ARB_LOST),             MP_ROM_INT(TWAI_ALERT_ARB_LOST)             },
        { MP_ROM_QSTR(MP_QSTR_ALERT_ABOVE_ERR_WARN),       MP_ROM_INT(TWAI_ALERT_ABOVE_ERR_WARN)       },
        { MP_ROM_QSTR(MP_QSTR_ALERT_BUS_ERROR),            MP_ROM_INT(TWAI_ALERT_BUS_ERROR)            },
        { MP_ROM_QSTR(MP_QSTR_ALERT_TX_FAILED),            MP_ROM_INT(TWAI_ALERT_TX_FAILED)            },
        { MP_ROM_QSTR(MP_QSTR_ALERT_RX_QUEUE_FULL),        MP_ROM_INT(TWAI_ALERT_RX_QUEUE_FULL)        },
        { MP_ROM_QSTR(MP_QSTR_ALERT_ERR_PASS),             MP_ROM_INT(TWAI_ALERT_ERR_PASS)             },
        { MP_ROM_QSTR(MP_QSTR_ALERT_BUS_OFF),              MP_ROM_INT(TWAI_ALERT_BUS_OFF)              }
    };
    
    static MP_DEFINE_CONST_DICT(twai_module_globals, twai_module_globals_table);
    
    
    const mp_obj_module_t twai_module = {
        .base    = {&mp_type_module},
        .globals = (mp_obj_dict_t *)&twai_module_globals,
    };
    
    MP_REGISTER_MODULE(MP_QSTR_twai, twai_module);
    
#endif /* SOC_TWAI_SUPPORTED */