CXX = g++
CXXFLAGS = -std=c++11 -Wall -Werror -Wno-pointer-arith -Wno-unused-variable
CXXFLAGS += -I./vendor

LIBS = -lGL

DEBUG ?= 1
ifeq ($(DEBUG), 1)
  CXXFLAGS += -O0 -gdwarf
  CMAKE_RELEASE=Debug
  BUILD_DIR = build/debug
else
  CXXFLAGS += -O2
  CMAKE_RELEASE=Release
  BUILD_DIR = build/release
endif

BIN = $(BUILD_DIR)/imgdb

GLFW_VERSION = 3.4
GLFW = $(BUILD_DIR)/libglfw-$(GLFW_VERSION).a
GLFW_PATH = ./vendor/glfw-$(GLFW_VERSION)
CXXFLAGS += -I$(GLFW_PATH)/include

IMGUI_VERSION = 1.92.2b
IMGUI = $(BUILD_DIR)/libimgui-$(IMGUI_VERSION).a
IMGUI_PATH = ./vendor/imgui-$(IMGUI_VERSION)
CXXFLAGS += -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends

all: libs $(BIN)

.PHONY: $(BIN)
$(BIN):
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LIBS) -o $@ ./src/main.cpp $(GLFW) $(IMGUI)

.PHONY: libs
libs: $(GLFW) $(IMGUI)

$(GLFW):
	mkdir -p $(BUILD_DIR)
	cmake -DCMAKE_BUILD_TYPE=$(CMAKE_RELEASE) \
		  -DGLFW_BUILD_DOCS=Off \
		  -DGLFW_BUILD_TESTS=Off \
		  -DGLFW_BUILD_EXAMPLES=Off \
		  -S $(GLFW_PATH) \
		  -B $(GLFW_PATH)/build
	$(MAKE) -C $(GLFW_PATH)/build
	mv $(GLFW_PATH)/build/src/libglfw3.a $(GLFW)


$(IMGUI):
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ -c vendor/imgui-all.cpp

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(GLFW_PATH)/build
