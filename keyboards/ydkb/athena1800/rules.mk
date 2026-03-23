# project specific files
SRC ?=	c1_display.c c1_main.c matrix.c user_led.c user_function.c user_rawhid.c

MCU_LDSCRIPT = RP2040_FLASH_TIMECRIT_16M

ALLOW_WARNINGS = yes

CUSTOM_MATRIX            = yes # Custom matrix file
CONSOLE_ENABLE          ?= no	# Console for debug

QUANTUM_PAINTER_ENABLE   = yes
QUANTUM_PAINTER_DRIVERS  = gc9107_spi

# Display data
SRC +=  gfx/robotomono20.qff.c \
        gfx/boot.qgf.c \
        gfx/boot2.qgf.c

# 16M FLASH
# LDFLAGS += -Xlinker --defsym=FLASH_LEN=16384k
OPT_DEFS += -DCRT0_EXTRA_CORES_NUMBER=1
OPT_DEFS += -DPICO_FLASH_SIZE_BYTES=16777216
