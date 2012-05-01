ifneq ($(TARGET_BOARD_PLATFORM),omap3)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    ColorConvert.cpp

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/base/include/media/stagefright/openmax \
        $(TOP)/frameworks/media/libvideoeditor/include

LOCAL_SHARED_LIBRARIES :=       \

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE := libI420colorconvert

include $(BUILD_HEAPTRACKED_SHARED_LIBRARY)

endif
