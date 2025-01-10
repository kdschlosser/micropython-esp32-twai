
#include "py/obj.h"
#include "py/runtime.h"

#ifndef __TWAI_MESSAGE_H__
    #define __TWAI_MESSAGE_H__

    #include "soc/soc_caps"

#if SOC_TWAI_SUPPORTED

    typedef struct _mp_twai_message_t {
        mp_obj_base_t base;
        twai_message_t message;
        mp_obj_t tx_callback;
    } mp_twai_message_t;

    extern const mp_obj_type_t mp_twai_message_type_t;

#endif /* SOC_TWAI_SUPPORTED */
#endif /* __TWAI_MESSAGE_H__ */