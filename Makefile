.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET   := boot
BUILD    := build
SOURCES  := source
INCLUDES :=
LIBS     := -lfat -lwiiuse -lbte -logc -lm

CFLAGS   = -g -O2 -Wall $(MACHDEP) $(INCLUDE)
LDFLAGS  = -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export OFILES   := $(CFILES:.c=.o)
export LD       := $(CC)
export INCLUDE  := $(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir)) \
                   -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC) -I$(PORTLIBS)/include
export LIBPATHS := -L$(LIBOGC_LIB) -L$(PORTLIBS)/lib

.PHONY: $(BUILD) clean run

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

run:
	wiiload $(OUTPUT).dol

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
	@echo linking ... $(notdir $@)
	@$(LD) $^ $(LDFLAGS) $(LIBPATHS) $(LIBS) -o $@

-include $(DEPENDS)

endif
