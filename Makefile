# ── Platform / architecture detection ────────────────────────────────────────
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifneq ($(UNAME_S),Linux)
  $(error This Makefile targets Linux only. Use Visual Studio / MSBuild for Windows.)
endif

PLATFORM := linux

ifeq ($(UNAME_M),aarch64)
  ARCH := aarch64
else
  ARCH := x86_64
endif

# ── Version ───────────────────────────────────────────────────────────────────
# On a tagged commit use the tag; otherwise use the short SHA.
VERSION ?= $(shell git describe --tags --exact-match 2>/dev/null || git rev-parse --short HEAD)

# ── Output ───────────────────────────────────────────────────────────────────
BUILDDIR := Build/$(PLATFORM)-$(ARCH)
TARGET   := $(BUILDDIR)/Goopie-Launcher.$(ARCH)

# ── Toolchain ─────────────────────────────────────────────────────────────────
CXX := g++
CC  := gcc

# ── CEF configuration ─────────────────────────────────────────────────────────
# Spotify provides pre-built CEF binaries. Override CEF_VERSION when calling
# `make download-cef` to select a specific build.
CEF_DIR     := Vendors/Chromium
CEF_VERSION ?= 148.0.9+g0d9d52a+chromium-148.0.7778.180

ifeq ($(ARCH),aarch64)
  CEF_PLATFORM := linuxarm64
else
  CEF_PLATFORM := linux64
endif
CEF_ARCHIVE  := cef_binary_$(CEF_VERSION)_$(CEF_PLATFORM).tar.bz2
CEF_URL      := https://cef-builds.spotifycdn.com/$(CEF_ARCHIVE)

# ── Compiler flags ────────────────────────────────────────────────────────────
CXXFLAGS := \
  -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter \
  -I Vendors \
  -I Vendors/GLFW/include \
  -I $(CEF_DIR)/include \
  -I $(CEF_DIR) \
  -DNDEBUG

CFLAGS := -O2 -I Vendors -I Vendors/GLFW/include

# ── Linker flags ──────────────────────────────────────────────────────────────
LDFLAGS := \
  -L $(CEF_DIR)/Release \
  -L $(CEF_DIR)/build/libcef_dll_wrapper \
  -lcef_dll_wrapper \
  -lcef \
  -lglfw \
  -lGL \
  -lcurl \
  -lpthread \
  -ldl \
  -lX11 \
  -Wl,-rpath,'$$ORIGIN'

# ── Sources ───────────────────────────────────────────────────────────────────
SRCS_CXX := \
  Source/NewLauncher.cpp \
  Source/Window/Window.cpp \
  Source/Networking/FileDownloader.cpp \
  Source/Utils/IsoExtraction.cpp \
  Vendors/stb_image/stb_image.cpp

SRCS_C := \
  Vendors/glad.c

OBJS_CXX := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(SRCS_CXX))
OBJS_C   := $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS_C))
OBJS     := $(OBJS_CXX) $(OBJS_C)

# ── Targets ───────────────────────────────────────────────────────────────────
.PHONY: all _build clean download-cef

all: _build

_build: $(TARGET)
	@echo "Build complete: $(TARGET)"

$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $^ $(LDFLAGS) -o $@
	@cp -r Assets $(BUILDDIR)/Assets 2>/dev/null || true
	@cp -n $(CEF_DIR)/Release/libcef.so $(BUILDDIR)/ 2>/dev/null || true
	@cp -n $(CEF_DIR)/Release/libEGL.so $(BUILDDIR)/ 2>/dev/null || true
	@cp -n $(CEF_DIR)/Release/libGLESv2.so $(BUILDDIR)/ 2>/dev/null || true
	@cp -n $(CEF_DIR)/Release/libvk_swiftshader.so $(BUILDDIR)/ 2>/dev/null || true
	@cp -n $(CEF_DIR)/Release/libvulkan.so.1 $(BUILDDIR)/ 2>/dev/null || true
	@cp -n $(CEF_DIR)/Release/v8_context_snapshot.bin $(BUILDDIR)/ 2>/dev/null || true
	@cp -n $(CEF_DIR)/Release/vk_swiftshader_icd.json $(BUILDDIR)/ 2>/dev/null || true
	@cp -rn $(CEF_DIR)/Resources/. $(BUILDDIR)/ 2>/dev/null || true

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR)

# ── Download and build CEF ────────────────────────────────────────────────────
# URL-encode '+' → '%2B' so curl fetches the right file.
CEF_URL_ENCODED := $(subst +,%2B,$(CEF_URL))
CEF_ARCHIVE_PATH := /tmp/$(subst /,_,$(CEF_ARCHIVE))

download-cef:
	@echo "==> Downloading CEF $(CEF_VERSION) for $(CEF_PLATFORM)..."
	@mkdir -p $(CEF_DIR)
	curl -L --fail --progress-bar -o "$(CEF_ARCHIVE_PATH)" "$(CEF_URL_ENCODED)"
	@echo "==> Extracting..."
	tar xjf "$(CEF_ARCHIVE_PATH)" --strip-components=1 -C "$(CEF_DIR)"
	@rm -f "$(CEF_ARCHIVE_PATH)"
	@echo "==> Marking xdvdfs executable..."
	@chmod +x Assets/xdvdfs 2>/dev/null || true
	@echo "==> Building libcef_dll_wrapper..."
	cmake -B $(CEF_DIR)/build -S $(CEF_DIR) \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DUSE_SANDBOX=OFF \
	  -DCEF_RUNTIME_LIBRARY_FLAG="" \
	  -G "Unix Makefiles"
	cmake --build $(CEF_DIR)/build --target libcef_dll_wrapper -- -j$$(nproc)
	@echo "==> CEF ready."
