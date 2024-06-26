include project/target/dartuinoP0.mk
include project/virtual/test.mk
include project/virtual/minip.mk

# Console serial port is on pins PA9(TX) and PB7(RX)

include project/virtual/fs.mk

DISPLAY_PANEL_TYPE ?= LS013B7DH06
ENABLE_DARTUINO_SENSOR_BUS := 1

MODULES += \
  target/dartuinoP0/projects/system

MODULE_DEPS += \
    app/accelerometer \

