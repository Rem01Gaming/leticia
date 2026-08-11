LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := leticia

LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_C_INCLUDES := $(LOCAL_PATH)/FFmpeg/include

LLOCAL_CFLAGS := -std=c++23 -g -O0 -fPIE -fPIC
LOCAL_CPPFLAGS := -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized
LOCAL_LDFLAGS := -fPIE -fPIC -Wl,-z,max-page-size=16384
LOCAL_LDLIBS := -lz -lm

LOCAL_STATIC_LIBRARIES := avformat avcodec swresample avutil

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := avformat
LOCAL_SRC_FILES := FFmpeg/lib/libavformat.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avcodec
LOCAL_SRC_FILES := FFmpeg/lib/libavcodec.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := swresample
LOCAL_SRC_FILES := FFmpeg/lib/libswresample.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avutil
LOCAL_SRC_FILES := FFmpeg/lib/libavutil.a
include $(PREBUILT_STATIC_LIBRARY)

