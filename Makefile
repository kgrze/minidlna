TARGET = minidlna

CFLAGS = -c -m64 -O0 -g -ggdb3 -Wall -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
LFLAGS = -lc -lpthread -lm -ljpeg -lid3tag -lsqlite3 -lavformat -lavutil -lexif -lFLAC -lvorbis -logg

SRCDIR = src
OBJDIR = obj
BINDIR = bin

LD = $(CC)

SOURCES  := $(wildcard $(SRCDIR)/*.c)
INCLUDES := $(wildcard $(SRCDIR)/*.h)
OBJECTS  := $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
rm       = rm -f

$(BINDIR)/$(TARGET): $(OBJECTS)
	@echo "Linking $@"
	@$(LD) $(OBJECTS) $(LFLAGS) -o $@

$(OBJECTS): $(OBJDIR)/%.o : $(SRCDIR)/%.c
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(rm) $(OBJECTS)
	$(rm) $(BINDIR)/$(TARGET)

help:
	@echo "Build following target:"
	@echo "$(TARGET)"