# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

FLAGS +=
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=
ifeq ($(OS),Windows_NT)
LDFLAGS += -lopengl32
endif

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk
