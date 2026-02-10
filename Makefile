all : flash

#TARGET:=eth_sfhip
#TARGET_MCU:=CH32V208
#TARGET_MCU_PACKAGE:=CH32V208WBU6
#EXTRA_CFLAGS += -DUSB_USE_USBD # for some specifically cheap v208 boards

TARGET:=usb_sfhip
TARGET_MCU:=CH582
TARGET_MCU_PACKAGE:=CH582F

#TARGET:=iSLER_sfhip
#TARGET_MCU:=CH570
#TARGET_MCU_PACKAGE:=CH570D

include ~/temp/ch32fun/ch32fun/ch32fun.mk

flash : cv_flash
clean : cv_clean
