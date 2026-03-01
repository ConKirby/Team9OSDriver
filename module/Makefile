# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Project Echo kernel module
#

obj-m := echo_robot.o

echo_robot-objs := echo_main.o \
                   echo_chardev.o \
                   echo_servo.o \
                   echo_joystick.o \
                   echo_state.o \
                   echo_buffer.o \
                   echo_proc.o

ccflags-y := -Wall -Wextra

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
