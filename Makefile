CC = aarch64-linux-gnu-gcc
LD = aarch64-linux-gnu-ld
CFLAGS = -ffreestanding -nostdlib -nostartfiles -mgeneral-regs-only -Wall -O0 -g

DEBUG_HOOKS ?= 0
ifeq ($(DEBUG_HOOKS),1)
CFLAGS += -DDEBUG_HOOKS
endif
CFLAGS += -MMD -MP

SMP_SELFTEST ?= 0
ifeq ($(SMP_SELFTEST),1)
CFLAGS += -DSMP_SELFTEST
endif

OBJS = boot/boot.o kernel/vectors.o kernel/uart.o kernel/switch.o kernel/task.o \
       kernel/gic.o kernel/sched.o kernel/sync.o kernel/pci.o kernel/accel.o kernel/psci.o kernel/smp.o kernel/main.o

build/kernel.elf: $(OBJS) linker.ld
	mkdir -p build
	$(LD) -T linker.ld -o $@ $(OBJS)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Point this at your own dma-accel-enabled QEMU build
QEMU = /home/kelvin/projects/qemu-src/build/qemu-system-aarch64

run: build/kernel.elf
	$(QEMU) -M virt -cpu cortex-a53 -nographic -device dma-accel -kernel build/kernel.elf

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) build/kernel.elf

-include $(OBJS:.o=.d)
