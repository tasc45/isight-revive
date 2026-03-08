# iSightRevive — FireWire iSight CMIO DAL Plugin
# Build with: make
# Install with: make install (requires sudo)

PLUGIN_NAME = iSightRevive
BUNDLE = $(PLUGIN_NAME).plugin
BINARY = $(PLUGIN_NAME)
INSTALL_DIR = /Library/CoreMediaIO/Plug-Ins/DAL

# Source files
SOURCES = Source/PluginMain.cpp \
          Source/PluginInterface.cpp \
          Source/Device.cpp \
          Source/Stream.cpp \
          Source/IIDCCamera.cpp \
          Source/FrameAssembler.cpp

# Compiler settings
CXX = clang++
CXXFLAGS = -std=c++17 -arch x86_64 \
           -mmacosx-version-min=14.0 \
           -fno-rtti \
           -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
           -ISource

# Objective-C++ for FrameAssembler (uses @{} literals)
OBJCXXFLAGS = $(CXXFLAGS) -ObjC++

# Frameworks
LDFLAGS = -bundle \
          -arch x86_64 \
          -mmacosx-version-min=14.0 \
          -framework CoreFoundation \
          -framework CoreMedia \
          -framework CoreMediaIO \
          -framework CoreVideo \
          -framework IOKit \
          -framework Foundation

# Build directory
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

# Object files
OBJECTS = $(OBJ_DIR)/PluginMain.o \
          $(OBJ_DIR)/PluginInterface.o \
          $(OBJ_DIR)/Device.o \
          $(OBJ_DIR)/Stream.o \
          $(OBJ_DIR)/IIDCCamera.o \
          $(OBJ_DIR)/FrameAssembler.o

# Default target
all: $(BUILD_DIR)/$(BUNDLE)

# Create the .plugin bundle
$(BUILD_DIR)/$(BUNDLE): $(BUILD_DIR)/$(BUNDLE)/Contents/MacOS/$(BINARY) $(BUILD_DIR)/$(BUNDLE)/Contents/Info.plist

$(BUILD_DIR)/$(BUNDLE)/Contents/MacOS/$(BINARY): $(OBJECTS) | $(BUILD_DIR)/$(BUNDLE)/Contents/MacOS
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS)

$(BUILD_DIR)/$(BUNDLE)/Contents/Info.plist: Resources/Info.plist | $(BUILD_DIR)/$(BUNDLE)/Contents
	cp $< $@

# Compile C++ sources
$(OBJ_DIR)/PluginMain.o: Source/PluginMain.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ_DIR)/PluginInterface.o: Source/PluginInterface.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ_DIR)/Device.o: Source/Device.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ_DIR)/Stream.o: Source/Stream.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ_DIR)/IIDCCamera.o: Source/IIDCCamera.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# FrameAssembler uses Obj-C++ for NSDictionary literals
$(OBJ_DIR)/FrameAssembler.o: Source/FrameAssembler.cpp | $(OBJ_DIR)
	$(CXX) $(OBJCXXFLAGS) -c -o $@ $<

# Directory creation
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BUILD_DIR)/$(BUNDLE)/Contents/MacOS:
	mkdir -p $@

$(BUILD_DIR)/$(BUNDLE)/Contents:
	mkdir -p $@

# Install
install: $(BUILD_DIR)/$(BUNDLE)
	sudo cp -R $(BUILD_DIR)/$(BUNDLE) $(INSTALL_DIR)/
	@echo "Installed to $(INSTALL_DIR)/$(BUNDLE)"
	@echo "Restart camera-using apps (or: sudo killall VDCAssistant IIDCVideoAssistant 2>/dev/null)"

# Uninstall
uninstall:
	sudo rm -rf $(INSTALL_DIR)/$(BUNDLE)
	@echo "Removed $(INSTALL_DIR)/$(BUNDLE)"

# Clean
clean:
	rm -rf $(BUILD_DIR)

# Debug build
debug: CXXFLAGS += -g -O0 -DDEBUG
debug: OBJCXXFLAGS += -g -O0 -DDEBUG
debug: all

# Release build
release: CXXFLAGS += -O2 -DNDEBUG
release: OBJCXXFLAGS += -O2 -DNDEBUG
release: all

.PHONY: all install uninstall clean debug release
