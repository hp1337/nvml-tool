# /home/himesh/nvml-tool/Makefile
CC = gcc
PREFIX = /usr/local

# Try pkg-config first for NVML (version-agnostic search)
PKG_CONFIG_NVML = $(shell pkg-config --list-all 2>/dev/null | grep -o 'nvidia-ml[^ ]*' | head -1)
ifneq ($(PKG_CONFIG_NVML),)
    NVML_CFLAGS = $(shell pkg-config --cflags $(PKG_CONFIG_NVML) 2>/dev/null)
    NVML_LIBS = $(shell pkg-config --libs $(PKG_CONFIG_NVML) 2>/dev/null)
else
    ifeq ($(NVML_CFLAGS),)
        $(error NVML not found via pkg-config. Please provide NVML_CFLAGS and NVML_LIBS. Example: make NVML_CFLAGS="-I/usr/local/cuda/include" NVML_LIBS="-L/usr/local/cuda/lib64 -lnvidia-ml")
    endif
    ifeq ($(NVML_LIBS),)
        $(error NVML not found via pkg-config. Please provide NVML_CFLAGS and NVML_LIBS. Example: make NVML_CFLAGS="-I/usr/local/cuda/include" NVML_LIBS="-L/usr/local/cuda/lib64 -lnvidia-ml")
    endif
endif

# Try pkg-config for libpci (required for VRAM temp access)
PKG_CONFIG_PCI = $(shell pkg-config --list-all 2>/dev/null | grep -o 'libpci' | head -1)
ifneq ($(PKG_CONFIG_PCI),)
    PCI_CFLAGS = $(shell pkg-config --cflags libpci 2>/dev/null)
    PCI_LIBS = $(shell pkg-config --libs libpci 2>/dev/null)
else
    # Fallback if pkg-config isn't listing libpci but it's installed
    PCI_CFLAGS =
    PCI_LIBS = -lpci
endif

CFLAGS = -Wall -Wextra -std=c99 -O2 $(NVML_CFLAGS) $(PCI_CFLAGS)
LDFLAGS = $(NVML_LIBS) $(PCI_LIBS)

# Directories
SRCDIR = src
BUILDDIR = build

TARGET = $(BUILDDIR)/nvml-tool
SOURCES = $(SRCDIR)/main.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

# Default target
all: $(TARGET)

# Build the main executable
$(TARGET): $(OBJECTS) | $(BUILDDIR)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# Compile source files
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create build directory
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Clean build artifacts
clean:
	rm -rf $(BUILDDIR)

# Install (default: /usr/local/bin, configurable with PREFIX)
install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/

# Uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

# Show detected paths
show-config:
	@echo "Configuration:"
	@echo "  NVML CFLAGS: $(NVML_CFLAGS)"
	@echo "  NVML LIBS:   $(NVML_LIBS)"
	@echo "  PCI CFLAGS:  $(PCI_CFLAGS)"
	@echo "  PCI LIBS:    $(PCI_LIBS)"

# Show help
help:
	@echo "Available targets:"
	@echo "  all         - Build the program (default)"
	@echo "  clean       - Remove build artifacts"
	@echo "  install     - Install to PREFIX/bin (default: /usr/local/bin)"
	@echo "  uninstall   - Remove from PREFIX/bin"
	@echo "  show-config - Show detected library paths"
	@echo "  help        - Show this help message"

.PHONY: all clean install uninstall show-config help

