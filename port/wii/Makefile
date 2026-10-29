#---------------------------------------------------------------------------------
# Wii build of the Initial D Arcade Stage 3 port (devkitPPC + libogc).
#
#   cd port/wii && make            -> dist/apps/initiald3wii/boot.dol
#                                     (boot.elf and its map stay in build/)
#
# Install: copy the contents of dist/ to the root of the SD card
# (apps/initiald3wii/ holds boot.dol, meta.xml and icon.png) and put your own
# gds-0033.chd, 317-0384-com.pic and naomi.zip (a NAOMI BIOS set) in
# sd:/initiald3/ (or usb:/initiald3/).
#
# Two halves are linked together:
#   - the launcher (../src): CHD/PIC/DIMM handling and the menus,
#   - the emulator core (../emu): the nullDC4Wii fork with the NAOMI 2
#     hardware layer (naomi/), built for DC_PLATFORM=2 with the Wii
#     back-ends (GX renderer, PPC dynarec, ASND audio, MMU fastmem).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET   := boot
BUILD    := build
CHDR     := ../third_party/libchdr
EMU      := ../emu

LAUNCHER_SOURCES := ../src ../src/core ../src/ui ../src/platform ../src/game
CHDR_SOURCES     := $(CHDR)/src \
                    $(CHDR)/deps/lzma-19.00 \
                    $(CHDR)/deps/zstd-1.5.6/lib/common \
                    $(CHDR)/deps/zstd-1.5.6/lib/decompress
CORE_SOURCES     := $(EMU) $(EMU)/config $(EMU)/cl $(EMU)/plugins \
                    $(EMU)/dc $(EMU)/dc/sh4 $(EMU)/dc/sh4/rec_v2 $(EMU)/dc/mem \
                    $(EMU)/dc/pvr $(EMU)/dc/aica $(EMU)/dc/asic $(EMU)/dc/maple \
                    $(EMU)/plugs/drkPvr $(EMU)/plugs/nullAICA $(EMU)/plugs/vbaARM \
                    $(EMU)/plugs/nullExtDev $(EMU)/naomi $(EMU)/host \
                    $(EMU)/wii $(EMU)/wii/dc/sh4/rec_v2
SOURCES  := $(LAUNCHER_SOURCES) $(CHDR_SOURCES) $(CORE_SOURCES)
INCLUDES := ../src $(EMU) $(CHDR)/include $(CHDR)/deps/zlib-1.3.1
DATA     :=

# zlib: only the inflate side is needed (listed explicitly, the folder also
# holds deflate sources that are not vendored).
ZLIB_C   := adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c

# Core files that are not part of the NAOMI 2 Wii build:
#   nullDC.cpp      the Dreamcast front end (naomi/naomi_frontend.cpp replaces it)
#   mem.cpp         nullAICA's old memory file, superseded by aica_mem.cpp
CORE_EXCLUDE := nullDC.cpp mem.cpp

#---------------------------------------------------------------------------------
# Flags
#---------------------------------------------------------------------------------
# DC_PLATFORM=2 selects the NAOMI 2 memory map in the core; _WII picks its Wii
# back-ends. The code-generation flags are nullDC4Wii's.
CFLAGS   := -g -O2 -Wall $(MACHDEP) $(INCLUDE) -D_7ZIP_ST -DZSTD_DISABLE_ASM \
            -D_WII -DRELEASE -DDC_PLATFORM=2 \
            -fno-strict-aliasing -fsingle-precision-constant -frename-registers \
            -fomit-frame-pointer -ffast-math
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-exceptions -fno-rtti
LDFLAGS  := -g $(MACHDEP) -Wl,-Map,$(TARGET).elf.map

# Third-party C code and the inherited core are built with fewer warnings;
# the launcher keeps the strict set.
THIRD_PARTY_CFLAGS := -w
CORE_CXXFLAGS      := -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-variable \
                      -Wno-unused-but-set-variable -Wno-sign-compare -Wno-parentheses
LAUNCHER_CXXFLAGS  := -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

LIBS     := -lwiiuse -lbte -lfat -lasnd -logc -lm
LIBDIRS  :=

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

# The .dol is written straight into the SD card layout; the .elf (symbols,
# for addr2line / objdump) stays with the objects.
export APPDIR  := $(CURDIR)/dist/apps/initiald3wii
export OUTPUT  := $(CURDIR)/$(BUILD)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(CURDIR)/$(CHDR)/deps/zlib-1.3.1
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(CHDR_SOURCES),$(notdir $(wildcard $(dir)/*.c))) $(ZLIB_C)
LAUNCHER_CPP    := $(foreach dir,$(LAUNCHER_SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
CORE_CPP        := $(filter-out $(CORE_EXCLUDE),$(foreach dir,$(CORE_SOURCES),$(notdir $(wildcard $(dir)/*.cpp))))
CPPFILES        := $(LAUNCHER_CPP) $(CORE_CPP)

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export THIRD_PARTY_OFILES := $(CFILES:.c=.o)
export CORE_OFILES := $(CORE_CPP:.cpp=.o)
export LAUNCHER_OFILES := $(LAUNCHER_CPP:.cpp=.o)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)
export LIBPATHS := -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean run package

# Builds dist/, the SD card layout: copy its contents to the root of the card.
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@mkdir -p $(APPDIR) dist/initiald3
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@cp meta.xml icon.png $(APPDIR)/
	@printf "Put your own gds-0033.chd, 317-0384-com.pic and naomi.zip (NAOMI BIOS set) in this folder.\r\n" > dist/initiald3/PUT_GAME_FILES_HERE.txt

# Kept for build_wii.bat and habits: the default target already lays out dist/.
package: $(BUILD)
	@echo packaged ... dist/

clean:
	@echo clean ...
	@rm -fr $(BUILD) dist

run:
	wiiload $(APPDIR)/$(TARGET).dol

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

$(APPDIR)/$(TARGET).dol: $(OUTPUT).elf
	$(SILENTMSG) output ... dist/apps/initiald3wii/$(notdir $@)
	$(SILENTCMD)elf2dol $< $@
$(OUTPUT).elf: $(OFILES)

$(THIRD_PARTY_OFILES): CFLAGS += $(THIRD_PARTY_CFLAGS)
$(CORE_OFILES): CXXFLAGS += $(CORE_CXXFLAGS)
$(LAUNCHER_OFILES): CXXFLAGS += $(LAUNCHER_CXXFLAGS)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
