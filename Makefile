.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Define DEVKITARM (export DEVKITARM=/opt/devkitpro/devkitARM)")
endif

include $(DEVKITARM)/3ds_rules

CTRULIB ?= $(DEVKITPRO)/libctru

TARGET   := ytmusic3ds
BUILD    := build
SOURCES  := source
INCLUDES := include

# Old 3DS: ARM11 MPCore (ARMv6K), sin NEON
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections -fomit-frame-pointer \
           $(ARCH) -D__3DS__ -I$(INCLUDES) -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -L$(CTRULIB)/lib
LIBS    := -lctru -lm

SRCS := $(wildcard $(SOURCES)/*.c)
OBJS := $(patsubst $(SOURCES)/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean

all: $(TARGET).3dsx

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: $(SOURCES)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(TARGET).smdh:
	smdhtool --create "YT Music 3DS" "Cliente de YouTube Music" "homebrew" $(CTRULIB)/default_icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	3dsxtool $< $@ --smdh=$(TARGET).smdh

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh
