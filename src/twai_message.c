#include "twai_message.h"
#include "twai_mod.h"
#include "py/runtime.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/binary.h"
#include "py/builtin.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "mpconfigport.h"

#include <string.h>

#include "soc/soc_caps.h"

#if SOC_TWAI_SUPPORTED

    static uint8_t _get_value(mp_obj_t value);
    static uint8_t _get_index(mp_twai_message_t *self, mp_obj_t index) ;
    static mp_obj_t twai_message_index(size_t n_args, const mp_obj_t *args);
    static mp_obj_t twai_message_pop(size_t n_args, const mp_obj_t *args);
    static mp_obj_t twai_message_append(mp_obj_t self_in, mp_obj_t value);

    twai_message_make_new

    static mp_obj_t twai_message_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
    {
        TWAI_UNUSED(n_args);
        TWAI_UNUSED(n_kw);
        TWAI_UNUSED(all_args);

        // create new object
        mp_twai_message_t *self = m_new_obj(mp_twai_message_t);
        self->base.type = &mp_twai_message_type_t;

        self->tx_callback = mp_const_none;

        return MP_OBJ_FROM_PTR(self);
    }


    static void twai_message_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

        if (dest[0] == MP_OBJ_NULL) {
            // getter
            switch(attr)
            {
                case MP_QSTR_is_extended:
                    dest[0] = mp_obj_new_bool((bool)self->message.extd);
                    break;

                case MP_QSTR_is_rtr:
                    dest[0] = mp_obj_new_bool((bool)self->message.rtr);
                    break;

                case MP_QSTR_id:
                    dest[0] = mp_obj_new_int_from_uint(self->message.identifier);
                    break;

                case MP_QSTR_data:
                    mp_obj_array_t *view = MP_OBJ_TO_PTR(mp_obj_new_memoryview(
                        BYTEARRAY_TYPECODE,
                        (size_t)self->message.data_length_code,
                        (void *)self->message.data
                    ));

                    // used to indicate writable buffer
                    view->typecode |= 0x80;

                    dest[0] = MP_OBJ_FROM_PTR(view);
                    break;

                case MP_QSTR_tx_callback:
                    dest[0] = self->tx_callback;
                    break;

                default:
                    // fallback to locals_dict lookup
                    const mp_obj_type_t *type = mp_obj_get_type(self_in);

                    while (MP_OBJ_TYPE_HAS_SLOT(type, locals_dict)) {
                        assert(MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->base.type == &mp_type_dict);

                        mp_map_t *locals_map = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
                        mp_map_elem_t *elem = mp_map_lookup(locals_map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);

                        if (elem != NULL) {
                            mp_convert_member_lookup(self_in, type, elem->value, dest);
                            break;
                        }
                        if (MP_OBJ_TYPE_GET_SLOT_OR_NULL(type, parent) == NULL) break;

                        // search parents
                        type = MP_OBJ_TYPE_GET_SLOT(type, parent);
                    }
            }
        } else {
            if (dest[1])
            {
                // setter
                switch(attr)
                {
                    case MP_QSTR_is_extended:
                        is_ext = (bool)mp_obj_get_int_truncated(dest[1]);
                        self->message.extd = is_ext ? 1 : 0;
                        break;

                    case MP_QSTR_is_rtr:
                        is_rtr = (bool)mp_obj_get_int_truncated(dest[1]);
                        self->message.rtr = is_rtr ? 1 : 0;
                        break;

                    case MP_QSTR_single_shot:
                        is_ss = (bool)mp_obj_get_int_truncated(dest[1]);
                        self->message.ss = is_ss ? 1 : 0;
                        break;

                    case MP_QSTR_id:
                        uint32_t id = (uint32_t)mp_obj_get_int_truncated(dest[1]);

                        if (self->message.extd) self->message.identifier = id & 0x1FFFFFFF;
                        else self->message.identifier = id & 0x7FF;
                        break;

                    case MP_QSTR_data:
                        mp_buffer_info_t bufinfo;
                        mp_get_buffer_raise(dest[1], &bufinfo, MP_BUFFER_READ);

                        if (bufinfo.len > TWAI_FRAME_MAX_DLC) {
                            // raise value error
                        }

                        memcpy((void *)self->message.data, bufinfo.buf, (size_t)bufinfo.len);

                        if (bufinfo.len > 8) self->message.dlc_non_comp = 1;
                        else self->message.dlc_non_comp = 0;

                        self->message.data_length_code = (uint8_t)bufinfo.len;
                        break;

                    case MP_QSTR_tx_callback:
                        if (dest[1] == mp_const_none || mp_obj_is_callable(dest[1])) {
                            self->tx_callback =  dest[1]
                        }
                        break;

                    default: return;
                }

                dest[0] = MP_OBJ_NULL; // indicate success
            }
        }
    }


    mp_obj_t twai_message_remove(mp_obj_t self_in, mp_obj_t value)
    {
        mp_obj_t args[] = {self_in, value};
        args[1] = twai_message_index(2, args);
        twai_message_pop(2, args);

        return mp_const_none;
    }

    static MP_DEFINE_CONST_FUN_OBJ_2(twai_message_remove_obj, twai_message_remove);


    static mp_obj_t twai_message_insert(mp_obj_t self_in, mp_obj_t index, mp_obj_t value)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

        uint8_t idx = _get_index(self, index);
        uint8_t val = _get_value(value);

        if (self->message.data_length_code + 1 > TWAI_FRAME_MAX_DLC) NLR_INDEX_ERROR("message is full");

        for (uint8_t i = self->message.data_length_code - 1; i > idx; i--) {
            self->message.data[i] = self->message.data[i - 1];
        }

        self->message.data[idx] = val;
        self->message.data_length_code++;

        return mp_const_none;
    }

    static MP_DEFINE_CONST_FUN_OBJ_3(twai_message_insert_obj, twai_message_insert);


    mp_obj_t twai_message_index(size_t n_args, const mp_obj_t *args)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(args[0]);

        mp_obj_t value = ;
        size_t start = 0;
        size_t stop = (size_t)self->message.data_length_code;

        uint8_t val = _get_value(args[1]);

        if (n_args >= 3) start = (size_t)mp_obj_get_int(args[2]);
        if (n_args >= 4) stop = (size_t)mp_obj_get_int(args[3]);

        for (size_t i = start; i < stop; i++) {
            if (self->message.data[i] == val) return MP_OBJ_NEW_SMALL_INT(i);
        }

        TWAI_VALUE_ERROR("not in sequence");
    }

    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(twai_message_index_obj, 2, 4, twai_message_index);


    static mp_obj_t twai_message_clear(mp_obj_t self_in)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(args[0]);
        self->message.data_length_code = 0;
        return mp_const_none;
    }

    static MP_DEFINE_CONST_FUN_OBJ_1(twai_message_clear_obj, twai_message_clear);


    mp_obj_t twai_message_pop(size_t n_args, const mp_obj_t *args)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(args[0]);

        if (self->message.data_length_code == 0) TWAI_INDEX_ERROR("pop from empty list");

        uint8_t index = 0;

        if (n_args == 1) index = self->message.data_length_code - 1;
        else index = _get_index(self, args[1]);

        mp_obj_t ret = mp_obj_new_int_from_uint(self->message.data[index]);
        self->message.data_length_code--;

        memmove(self->message.data + index, self->message.data + index + 1, self->message.data_length_code - index);
        return ret;
    }

    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(twai_message_pop_obj, 1, 2, twai_message_pop);


    static mp_obj_t twai_message_extend(mp_obj_t self_in, mp_obj_t arg_in)
    {
        mp_buffer_info_t bufinfo;
        if (mp_get_buffer(arg_in, &bufinfo, MP_BUFFER_READ)) {
            mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

            if (self->message.data_length_code + (uint8_t)bufinfo.len > TWAI_FRAME_MAX_DLC) TWAI_INDEX_ERROR("Message data is full");

            memcpy(self->message.data + self->message.data_length_code, bufinfo.buf, bufinfo.len);
            self->message.data_length_code += (uint8_t)bufinfo.len;
        } else {
            TWAI_VALUE_ERROR("Invalid data type");
        }
        return mp_const_none; // return None, as per CPython
    }

    static MP_DEFINE_CONST_FUN_OBJ_2(twai_message_extend_obj, twai_message_extend);


    mp_obj_t twai_message_append(mp_obj_t self_in, mp_obj_t value)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

        if (self->message.data_length_code + 1 > TWAI_FRAME_MAX_DLC) TWAI_INDEX_ERROR("Message data is full");

        uint8_t val = _get_value(value);

        self->message.data[self->message.data_length_code++] = val;
        return mp_const_none; // return None, as per CPython
    }

    static MP_DEFINE_CONST_FUN_OBJ_2(twai_message_append_obj, twai_message_append);


    static mp_obj_t twai_message_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

        if (value == MP_OBJ_NULL) {
            // delete
            #if MICROPY_PY_BUILTINS_SLICE
            if (mp_obj_is_type(index, &mp_type_slice)) {
                mp_bound_slice_t slice;
                if (!mp_seq_get_fast_slice_indexes(self->message.data_length_code, index, &slice)) {
                    mp_raise_NotImplementedError(NULL);
                }

                size_t len_adj = slice.start - slice.stop;
                assert(len_adj <= 0);

                mp_seq_replace_slice_no_grow(self->message.data, self->message.data_length_code,
                                slice.start, slice.stop, self->message.data, 0, 1);


                self->message.data_length_code += (uint8_t)len_adj;
                return mp_const_none;
            }
            #endif

            mp_obj_t args[2] = {self_in, index};
            twai_message_pop(2, args);

            return mp_const_none;

        } else if (value == MP_OBJ_SENTINEL) {
            #if MICROPY_PY_BUILTINS_SLICE
            if (mp_obj_is_type(index, &mp_type_slice)) {
                mp_bound_slice_t slice;

                if (!mp_seq_get_fast_slice_indexes(
                              self->message.data_length_code, index, &slice)) {

                    uint8_t start = (uint8_t)indexes->start
                    uint8_t stop = (uint8_t)indexes->stop;
                    uint8_t step = (uint8_t)indexes->step;

                    mp_obj_t res = mp_obj_new_list(0, NULL);

                    if (step < 0) {
                        while (start >= stop) {
                            mp_obj_list_append(res,
                                mp_obj_new_int_from_uint(self->message.data[start]));
                            start += step;
                        }
                    } else {
                        while (start < stop) {
                            mp_obj_list_append(res,
                                mp_obj_new_int_from_uint(self->message.data[start]));
                            start += step;
                        }
                    }

                    mp_obj_list_t *list = MP_OBJ_TO_PTR(res);
                    uint8_t buf[list->len] = { 0x00 };

                    for (size_t i=0;i<list->len;i++) {
                        buf[i] = (uint8_t)mp_obj_get_int_truncated(list->items[i]);
                    }

                    return mp_obj_new_bytearray(list->len, (const void *)buf);

                } else {
                    return mp_obj_new_bytearray(slice.stop - slice.start,
                                    (const void *)self->message.data + slice.start);
                }
            }
            #endif

            uint8_t i = _get_index(self, index);
            return mp_obj_new_int_from_uint(self->message.data[i]);

        } else {
            mp_buffer_info_t bufinfo;
            if (!mp_get_buffer(value, &bufinfo, MP_BUFFER_READ)) NLR_VALUEERROR("invalid data type");

            #if MICROPY_PY_BUILTINS_SLICE
            if (mp_obj_is_type(index, &mp_type_slice)) {
                mp_bound_slice_t slice_out;

                if (!mp_seq_get_fast_slice_indexes(self->message.data_length_code, index, &slice_out)) {
                    mp_raise_NotImplementedError(NULL);
                }

                size_t len_adj = bufinfo.len - (slice_out.stop - slice_out.start);

                if (self->message.data_length_code + len_adj > TWAI_FRAME_MAX_DLC) TWAI_VALUE_ERROR("Data is too long");

                if (len_adj > 0) {
                    mp_seq_replace_slice_grow_inplace(self->message.data, TWAI_FRAME_MAX_DLC,
                                slice_out.start, slice_out.stop, bufinfo.items,
                                bufinfo.len, len_adj, 1);
                } else {
                    mp_seq_replace_slice_no_grow(self->message.data, self->message.data_length_code,
                                slice_out.start, slice_out.stop, bufinfo.items, bufinfo.len, 1);

                }
                self->message.data_length_code += (uint8_t)len_adj;
                return mp_const_none;
            }
            #endif
            uint8_t i = _get_index(self, index);
            uint8_t val = _get_value(value);
            self->message.data[i] = val;
            return mp_const_none;
        }
    }


    static mp_obj_t twai_message_binary_op(mp_binary_op_t op, mp_obj_t lhs, mp_obj_t rhs)
    {

        if (!mp_obj_is_type(lhs, &mp_twai_message_type_t)) {
            return MP_OBJ_NULL; // op not supported
        }

        mp_twai_message_t *self = MP_OBJ_TO_PTR(lhs);

        switch (op) {
            case MP_BINARY_OP_INPLACE_ADD:
            case MP_BINARY_OP_ADD:
                mp_buffer_info_t bufinfo;

                if (!mp_get_buffer(rhs, &bufinfo, MP_BUFFER_READ)) return MP_OBJ_NULL; // op not supported
                if (((size_t)self->message.data_length_code + bufinfo.len) > TWAI_FRAME_MAX_DLC) TWAI_VALUE_ERROR("Data is too long");

                memcpy((void *)self->message.data + self->message.data_length_code), bufinfo.buf, bufinfo.len)

                return lhs;

            case MP_BINARY_OP_EQUAL:
                if (!mp_obj_is_type(rhs, &mp_twai_message_type_t)) return mp_const_false;

                mp_twai_message_t *rhs_inst = MP_OBJ_TO_PTR(rhs);

                if (rhs_inst->message.identifier != self->message.identifier) return mp_const_false;
                if (rhs_inst->message.data_length_code != self->message.data_length_code) return mp_const_false;
                if (rhs_inst->message.extd != self->message.extd) return mp_const_false;
                if (rhs_inst->message.rtr != self->message.rtr) return mp_const_false;

                for (uint8_t i=0;i<self->message.data_length_code;i++) {
                    if (self->message.data[i] != rhs_inst->message.data[i]) return mp_const_false;
                }

                return mp_const_true;
            default:
                return MP_OBJ_NULL; // op not supported
        }
    }


    static mp_obj_t twai_message_unary_op(mp_unary_op_t op, mp_obj_t self_in)
    {
        mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

        switch (op) {
            case MP_UNARY_OP_BOOL:
                return mp_obj_new_bool(self->message.data_length_code != 0);
            case MP_UNARY_OP_LEN:
                return MP_OBJ_NEW_SMALL_INT(self.message.data_length_code);
            default:
                // op not supported
                return MP_OBJ_NULL;
        }
    }


    static mp_int_t twai_message_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags)
    {
        TWAI_UNUSED(flags);
        mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

        bufinfo->buf = (void *)self->message.data;
        bufinfo->len = (size_t)self->message.data_length_code;
        bufinfo->typecode = BYTEARRAY_TYPECODE;
        return 0;
    }


    typedef struct _twai_message_it_t {
        mp_obj_base_t base;
        mp_twai_message_t *array;
        size_t offset;
        size_t cur;
    } twai_message_it_t;


    static mp_obj_t twai_message_it_iternext(mp_obj_t self_in)
    {
        twai_message_it_t *self = MP_OBJ_TO_PTR(self_in);

        if (self->cur < (size_t)self->array->message.data_length_code) {
            return mp_obj_new_int_from_uint(self->array->message.data[self->offset + self->cur++]);
        } else {
            return MP_OBJ_STOP_ITERATION;
        }
    }


    static MP_DEFINE_CONST_OBJ_TYPE(
        twai_message_it_type,
        MP_QSTR_twai_message_iterator,
        MP_TYPE_FLAG_ITER_IS_ITERNEXT,
        iter, twai_message_it_iternext
    );


    static mp_obj_t twai_message_iterator_new(mp_obj_t self_in, mp_obj_iter_buf_t *iter_buf)
    {

        mp_twai_message_t *self = MP_OBJ_TO_PTR(self_in);

        twai_message_it_t *o = (twai_message_it_type *)iter_buf;
        o->base.type = &twai_message_it_type;
        o->array = self;
        o->offset = 0;
        o->cur = 0;
        #if MICROPY_PY_BUILTINS_MEMORYVIEW
        return MP_OBJ_FROM_PTR(o);
    }



    static const mp_rom_map_elem_t twai_message_locals_dict_table[] = {
        { MP_ROM_QSTR(MP_QSTR_pop),    MP_ROM_PTR(&twai_message_pop_obj)    },
        { MP_ROM_QSTR(MP_QSTR_append), MP_ROM_PTR(&twai_message_append_obj) },
        { MP_ROM_QSTR(MP_QSTR_insert), MP_ROM_PTR(&twai_message_insert_obj) },
        { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&twai_message_remove_obj) },
        { MP_ROM_QSTR(MP_QSTR_clear),  MP_ROM_PTR(&twai_message_clear_obj)  },
        { MP_ROM_QSTR(MP_QSTR_extend), MP_ROM_PTR(&twai_message_extend_obj) },
        { MP_ROM_QSTR(MP_QSTR_index),  MP_ROM_PTR(&twai_message_index_obj)  }
    };

    MP_DEFINE_CONST_DICT(twai_message_locals_dict, twai_message_locals_dict_table);


    static MP_DEFINE_CONST_OBJ_TYPE(
        mp_twai_message_type_t,
        MP_QSTR_Message,
        MP_TYPE_FLAG_NONE,  MP_TYPE_FLAG_ITER_IS_GETITER
        make_new, twai_message_make_new,
        subscr, twai_message_subscr,
        attr, twai_message_attr,
        locals_dict, &twai_message_locals_dict,
        buffer, twai_message_get_buffer,
        iter, twai_message_iterator_new,
        unary_op, twai_message_unary_op,
        binary_op, twai_message_binary_op,
    );


    uint8_t _get_value(mp_obj_t value)
    {
        if (!mp_obj_is_type(value, &mp_type_int)) TWAI_VALUE_ERROR("value is not an integer type");
        int val = mp_obj_get_int(value);
        if (val < 0 || val > 255) TWAI_VALUE_ERROR("integer must be 1 byte in size");
        return (uint8_t)val;
    }


    uint8_t _get_index(mp_twai_message_t *self, mp_obj_t index)
    {
        if (!mp_obj_is_type(index, &mp_type_int)) TWAI_VALUE_ERROR("index is not an integer type");

        int i = (int)mp_obj_get_int(index);
        if (i < 0) i = (int)self->message.data_length_code + i;

        if (i >= (int)self->message.data_length_code) TWAI_INDEX_ERROR("Invalid index");
        return (uint8_t)i;
    }

#endif /* SOC_TWAI_SUPPORTED */