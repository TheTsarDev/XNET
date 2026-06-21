# XNET XBE Makefile
# Requires NXDK: https://github.com/XboxDev/nxdk

NXDK_DIR ?= $(HOME)/nxdk
XBE_TITLE = XNET
XISO_DIR  = $(CURDIR)/iso

# ── SOURCES ─────────────────────────────────────────────────────────────────────
SRCS = \
    $(CURDIR)/main.c \
    $(CURDIR)/xnet_crypto.c \
    $(CURDIR)/xnet_net.c \
    $(CURDIR)/xnet_ui.c \
    $(CURDIR)/xnet_files.c \
    $(CURDIR)/xnet_video.c \
    $(CURDIR)/xnet_log.c \
    $(CURDIR)/xnet_voice.c \
    $(CURDIR)/vendor/aes.c \
    $(CURDIR)/vendor/picojpeg.c

# ── CAMERA BACKEND ──────────────────────────────────────────────────────────────
# Real EyeToy/OV519 driver by default. It needs the USB stack (libusbohci), which
# is only built when audio is enabled — so with XNET_NO_AUDIO, or when you ask for
# it explicitly via XNET_CAM_STUB=1, fall back to the test-pattern stub.
ifdef XNET_NO_AUDIO
    SRCS += $(CURDIR)/xnet_camera_stub.c
else ifdef XNET_CAM_STUB
    SRCS += $(CURDIR)/xnet_camera_stub.c
else
    SRCS += $(CURDIR)/xnet_camera_nxdk.c
endif

ifdef XNET_NO_AUDIO
    SRCS += $(CURDIR)/xnet_audio_stub.c
else
    SRCS += $(CURDIR)/xnet_audio.c \
            $(CURDIR)/xnet_xblc.c
    CFLAGS += -I$(NXDK_DIR)/lib/usb/libusbohci/inc \
              -I$(NXDK_DIR)/lib/usb/libusbohci_xbox
endif

# ── NXDK FEATURES (must be set BEFORE include) ──────────────────────────────────
NXDK_NET    = y
NXDK_SDL    = y

ifndef XNET_NO_AUDIO
NXDK_USB    = y
endif

# ── INCLUDE NXDK (reads feature flags above) ────────────────────────────────────
include $(NXDK_DIR)/Makefile

# ── EXTRA FLAGS (set AFTER include) ─────────────────────────────────────────────
CFLAGS += -I$(CURDIR) -I$(CURDIR)/vendor
CFLAGS += -DAES128=1 -DCBC=1 -DECB=0 -DAES_BLOCKLEN=16

ifdef XNET_NO_AUDIO
CFLAGS += -DXNET_NO_AUDIO=1
endif

# ── XISO ────────────────────────────────────────────────────────────────────────
.PHONY: xiso
xiso: $(OUTPUT)
	@mkdir -p $(XISO_DIR)
	cp $(OUTPUT) $(XISO_DIR)/default.xbe
	$(NXDK_DIR)/tools/extract-xiso/build/extract-xiso -c $(XISO_DIR) xnet.iso
