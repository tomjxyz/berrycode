# berrycode - command-line coding agent for BB10 terminals (Term49).
# Builds a plain executable (no .bar): you copy it to the phone and run it in a shell.
#
#   make            phone binary          -> arm/berrycode
#   make sim        simulator binary      -> x86/berrycode
#   make both       both
#   make dist       phone bundle          -> berrycode-arm.tar.gz (binary + cacert.pem + sample config)
#   make check      static checks (PIE, libs + symbols resolve against the device sysroot)
#   make clean
ifndef QNX_TARGET
$(error QNX_TARGET not set - run: source ~/bbndk/bbndk-env.sh)
endif

APP  := berrycode
SRCS := main.c api.c tools.c json.c util.c spinner.c lineedit.c
HDRS := bc.h json.h util.h
LIBS := -lcurl -lm

ARCH ?= arm
ifeq ($(ARCH),arm)
TRIPLE := arm-unknown-nto-qnx8.0.0eabi
else ifeq ($(ARCH),x86)
TRIPLE := i486-pc-nto-qnx8.0.0
else
$(error ARCH must be arm or x86)
endif

CC      := $(TRIPLE)-gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu99 -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS := -pie -Wl,-z,relro -Wl,-z,now
OUT     := $(ARCH)
OBJS    := $(addprefix $(OUT)/obj/,$(SRCS:.c=.o))

all: $(OUT)/$(APP)

sim:
	@$(MAKE) --no-print-directory ARCH=x86
both:
	@$(MAKE) --no-print-directory ARCH=arm
	@$(MAKE) --no-print-directory ARCH=x86

$(OUT)/$(APP): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

$(OUT)/obj/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Mozilla CA bundle from this machine: the phone's own root store is from ~2015
cacert.pem:
	cp /etc/ssl/certs/ca-certificates.crt $@

dist: arm/$(APP) cacert.pem berrycode.conf.example README.md
	rm -rf dist && mkdir -p dist/berrycode
	cp arm/$(APP) cacert.pem berrycode.conf.example README.md dist/berrycode/
	tar -C dist -czf $(APP)-arm.tar.gz berrycode
	@echo "-> $(APP)-arm.tar.gz"

check: arm/$(APP)
	../../tools/check-bin.sh arm/$(APP)
	@if [ -f x86/$(APP) ]; then ../../tools/check-bin.sh x86/$(APP); fi

clean:
	rm -rf arm x86 dist $(APP)-arm.tar.gz

.PHONY: all sim both dist check clean
