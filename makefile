OUT      := out
SRC_DIR  := jni
TARGET   := $(OUT)/leticia

CXX      := clang++
CXXFLAGS := -std=c++23 -g -O0 -fPIE -fPIC -Wall -Wextra -Wpedantic
LDFLAGS  := -fPIE -fPIC
LDLIBS   := -lavformat -lavcodec -lswresample -lavutil -lz -lm -lpthread

PKG_CONFIG := pkg-config
ifneq ($(shell command -v $(PKG_CONFIG) 2>/dev/null),)
    CXXFLAGS += $(shell $(PKG_CONFIG) --cflags libavformat libavcodec libswresample libavutil 2>/dev/null)
    LDLIBS := $(shell $(PKG_CONFIG) --libs libavformat libavcodec libswresample libavutil 2>/dev/null) -lz -lm -lpthread
else
    CXXFLAGS += -I/usr/include/ffmpeg
endif

CXXFLAGS += -I$(SRC_DIR)

CXXFLAGS += -MMD -MP

SRCS := $(shell find $(SRC_DIR) -type f -name '*.cpp' 2>/dev/null)

OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OUT)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS) | $(OUT)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: $(SRC_DIR)/%.cpp | $(OUT)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

-include $(DEPS)

.DELETE_ON_ERROR:

.PHONY: all clean
