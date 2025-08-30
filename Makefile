CXX = g++
CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -pedantic
CXXFLAGS += -I./third-party -I./third-party/glfw/include

LIBS = -lGL

DEBUG ?= 0
ifeq ($(DEBUG), 1)
  CXXFLAGS += -O0 -ggdb
  CMAKE_RELEASE=Debug
  BUILD_DIR = build/debug
else
  CXXFLAGS += -O2
  CMAKE_RELEASE=Release
  BUILD_DIR = build/release
endif

BIN = $(BUILD_DIR)/tug

SAN ?= 0
ifeq ($(SAN), 1)
  CXXFLAGS += -fno-omit-frame-pointer -fsanitize=undefined,address
endif

# GLFW
GLFW = $(BUILD_DIR)/lib/libglfw3.a
GLFW_PATH = ./third-party/glfw

# ImGUI
IMGUI = $(BUILD_DIR)/libimgui.a

all: libs $(BIN)

.PHONY: $(BIN)
$(BIN):
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LIBS) -o $@ ./src/main.cpp $(GLFW) $(IMGUI)

.PHONY: libs
libs: $(GLFW) $(IMGUI)

$(GLFW):
	cmake -DCMAKE_BUILD_TYPE=$(CMAKE_RELEASE) \
		  -DCMAKE_INSTALL_PREFIX=$(BUILD_DIR) \
		  -DGLFW_BUILD_DOCS=Off \
		  -DGLFW_BUILD_TESTS=Off \
		  -DGLFW_BUILD_EXAMPLES=Off \
		  -S $(GLFW_PATH) \
		  -B $(GLFW_PATH)/build
	$(MAKE) -C $(GLFW_PATH)/build install

$(IMGUI):
	$(CXX) $(CXXFLAGS) -o $@ -c ./third-party/imgui/all.cpp

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
