all : flash

APP_CONFIG := isler
MCU_CONFIG := ch570
DEBUGPRINTF := 1

ifneq ($(filter eth,$(MAKECMDGOALS)),)
    APP_CONFIG := eth
endif
ifneq ($(filter usb,$(MAKECMDGOALS)),)
    APP_CONFIG := usb
    DEBUGPRINTF := 0
endif
ifneq ($(filter isler,$(MAKECMDGOALS)),)
    APP_CONFIG := isler
endif

ifneq ($(filter v208 ch32v208,$(MAKECMDGOALS)),)
    MCU_CONFIG := v208
endif
ifneq ($(filter ch582,$(MAKECMDGOALS)),)
    MCU_CONFIG := ch582
endif
ifneq ($(filter ch570,$(MAKECMDGOALS)),)
    MCU_CONFIG := ch570
endif

ifeq ($(APP_CONFIG),eth)
    TARGET := eth_sfhip
else ifeq ($(APP_CONFIG),usb)
    TARGET := usb_sfhip
else ifeq ($(APP_CONFIG),isler)
    TARGET := iSLER_sfhip
endif

ifeq ($(MCU_CONFIG),v208)
    TARGET_MCU := CH32V208
    TARGET_MCU_PACKAGE := CH32V208WBU6
    EXTRA_CFLAGS += -DUSB_USE_USBD # for some specifically cheap v208 boards
else ifeq ($(MCU_CONFIG),ch582)
    TARGET_MCU := CH582
    TARGET_MCU_PACKAGE := CH582F
else ifeq ($(MCU_CONFIG),ch570)
    TARGET_MCU := CH570
    TARGET_MCU_PACKAGE := CH570D
endif

CONFIG_TARGETS := eth usb isler v208 ch32v208 ch582 ch570
.PHONY: $(CONFIG_TARGETS)
$(CONFIG_TARGETS):
	@:

include ~/temp/ch32fun/ch32fun/ch32fun.mk
CFLAGS+=-DDEBUGPRINTF=$(DEBUGPRINTF)

flash : cv_flash
clean : cv_clean
