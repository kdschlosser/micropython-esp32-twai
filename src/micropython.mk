_MOD_DIR := $(USERMOD_DIR)
SRC_USERMOD += $(addprefix $(_MOD_DIR)/, twai_mod.c)
SRC_USERMOD += $(addprefix $(_MOD_DIR)/, twai_message.c)
CFLAGS_USERMOD += -I$(_MOD_DIR)
