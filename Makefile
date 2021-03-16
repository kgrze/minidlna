TARGET = minidlna

CFLAGS_COV = -c -m64 -O0 -g -ggdb3 -Wall -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -fprofile-arcs -ftest-coverage
LFLAGS_COV = -lpthread -ljpeg -lsqlite3 -lavformat -lavutil -lexif -fprofile-arcs -ftest-coverage

CFLAGS = -c -m64 -O0 -g -ggdb3 -Wall -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
LFLAGS = -lpthread -ljpeg -lsqlite3 -lavformat -lavutil -lexif

SRCDIR = src
BINDIR = bin

LD = $(CC)

SOURCES  := $(wildcard $(SRCDIR)/*.c)
INCLUDES := $(wildcard $(SRCDIR)/*.h)
OBJECTS  := $(SOURCES:$(SRCDIR)/%.c=$(SRCDIR)/%.o)
rm       = rm -rf

$(TARGET): $(OBJECTS)
	@echo "Linking $@"
	@$(LD) $(OBJECTS) $(LFLAGS_COV) -o $@

$(OBJECTS): $(SRCDIR)/%.o : $(SRCDIR)/%.c
	@echo "Compiling $<"
	@$(CC) $(CFLAGS_COV) -c $< -o $@

clean:
	$(rm) $(OBJECTS)
	$(rm) $(SRCDIR)/*.gcda
	$(rm) $(SRCDIR)/*.gcno
	$(rm) $(TARGET)
	$(rm) cache

help:
	@echo "Build following target:"
	@echo "$(TARGET)"

#lcov --c --directory ./src --output-file coverage.inf
#genhtml coverage.info --output-directory ./cov