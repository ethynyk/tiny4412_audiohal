LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_SRC_FILES:= \
	audio_hal.c
#	AudioHardware.cpp

LOCAL_MODULE := audio.primary.$(TARGET_DEVICE)
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_STATIC_LIBRARIES:= libmedia_helper
LOCAL_SHARED_LIBRARIES:= \
	libutils \
	liblog \
	libhardware_legacy \
	libtinyalsa \
	libaudioutils

#LOCAL_WHOLE_STATIC_LIBRARIES := libaudiohw_legacy
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES += libdl
LOCAL_C_INCLUDES += \
	external/tinyalsa/include \
	system/media/audio_effects/include \
	system/media/audio_utils/include \
	device/friendly-arm/$(TARGET_DEVICE)/conf

ifeq ($(strip $(BOARD_USES_I2S_AUDIO)),true)
  LOCAL_CFLAGS += -DUSES_I2S_AUDIO
endif

ifeq ($(strip $(BOARD_USES_PCM_AUDIO)),true)
  LOCAL_CFLAGS += -DUSES_PCM_AUDIO
endif

ifeq ($(strip $(BOARD_USES_SPDIF_AUDIO)),true)
  LOCAL_CFLAGS += -DUSES_SPDIF_AUDIO
endif

ifeq ($(strip $(USE_ULP_AUDIO)),true)
  LOCAL_CFLAGS += -DUSE_ULP_AUDIO
endif

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := AudioPolicyManager.cpp
LOCAL_SHARED_LIBRARIES := libcutils libutils
LOCAL_STATIC_LIBRARIES := libmedia_helper
LOCAL_WHOLE_STATIC_LIBRARIES := libaudiopolicy_legacy
LOCAL_MODULE := audio_policy.$(TARGET_DEVICE)
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional

ifeq ($(BOARD_HAVE_BLUETOOTH),true)
  LOCAL_CFLAGS += -DWITH_A2DP
endif

include $(BUILD_SHARED_LIBRARY)
