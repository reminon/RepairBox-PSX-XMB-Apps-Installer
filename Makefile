FLAVOR ?= USB-MX4SIO
ifeq ($(FLAVOR),MMCE)
BUILD_MMCE = 1
SOURCE_IRX = mmceman
else ifeq ($(FLAVOR),USB-MX4SIO)
BUILD_MMCE = 0
SOURCE_IRX = bdm bdmfs_fatfs mx4sio_bd usbd usbmass_bd
else
$(error FLAVOR must be MMCE or USB-MX4SIO)
endif

BUILD_DIR = build/$(FLAVOR)
EE_BIN = $(BUILD_DIR)/installer-symbols.elf
RELEASE_ELF = RepairBox.pl-PSX-XMB-Dynamic-App-Installer-v1.1-$(FLAVOR).elf

CORE = src/main src/ui src/sha256 src/source_media src/iop_module_lookup \
	src/apps/apps_ui src/apps/storage src/apps/system_version \
	src/apps/app_installer src/apps/game_installer
COMMON_IRX = iomanX fileXio sio2man padman ps2dev9 ps2atad ps2fs
IRX = $(COMMON_IRX) $(SOURCE_IRX)
EE_OBJS = $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(CORE))) \
	$(addprefix $(BUILD_DIR)/,$(addsuffix _irx.o,$(IRX))) \
	$(BUILD_DIR)/ps2hdd_psx1_irx.o $(BUILD_DIR)/default_cover_png.o
EE_LIBS = -lfileXio -lpad -ldebug -lpatches
EE_INCS = -Iinclude -Iinclude/apps -Ithird_party/hdl-dump \
	-I$(PS2SDK)/ports/include
EE_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -fdata-sections \
	-ffunction-sections -DRBX_BUILD_MMCE=$(BUILD_MMCE)
EE_LDFLAGS = -Wl,--gc-sections

.PHONY: all clean host-test safety-check usb-release mmce-release release
all: usb-release mmce-release
usb-release mmce-release: host-test safety-check
usb-release:
	$(MAKE) --no-print-directory FLAVOR=USB-MX4SIO release
mmce-release:
	$(MAKE) --no-print-directory FLAVOR=MMCE release
release: $(RELEASE_ELF)
$(RELEASE_ELF): $(EE_BIN)
	$(EE_STRIP) --strip-all -o $@ $<
	python3 tests/check_release.py $(FLAVOR) $@

host-test:
	python3 tests/test_separation.py

safety-check:
	python3 tests/test_source_safety.py

clean:
	rm -rf build
	rm -f RepairBox.pl-PSX-XMB-Dynamic-App-Installer-v1.1-USB-MX4SIO.elf
	rm -f RepairBox.pl-PSX-XMB-Dynamic-App-Installer-v1.1-MMCE.elf

$(BUILD_DIR)/src/%.o: src/%.c $(wildcard include/*.h) $(wildcard include/apps/*.h)
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(BUILD_DIR)/ps2hdd_psx1_irx.c: irx/psx1/ps2hdd-sparse-skip.irx
	@mkdir -p $(@D)
	$(PS2SDK)/bin/bin2c $< $@ ps2hdd_psx1_irx

$(BUILD_DIR)/default_cover_png.c: assets/default-cover.png
	@mkdir -p $(@D)
	$(PS2SDK)/bin/bin2c $< $@ default_cover_png

$(BUILD_DIR)/mmceman_irx.c: irx/mmceman.irx
	@mkdir -p $(@D)
	$(PS2SDK)/bin/bin2c $< $@ mmceman_irx

$(BUILD_DIR)/%_irx.c: irx/pinned/%.irx
	@mkdir -p $(@D)
	$(PS2SDK)/bin/bin2c $< $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
