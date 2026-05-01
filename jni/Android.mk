LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := leticia

LOCAL_SRC_FILES := \
	Main.cpp \
	Alsa.cpp \
	Resampler.cpp \
	SoftwareMixer.cpp \
	Replaygain.cpp \
	TinyAlsa.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)/FFmpeg/include

LOCAL_CFLAGS := -std=c++23 -O2 -fPIE -fPIC -flto
LOCAL_CPPFLAGS := -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

LOCAL_LDFLAGS := -fPIE -fPIC -Wl,-z,max-page-size=16384 -flto
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

