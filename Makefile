CXX ?= g++
CPPFLAGS += -Iinclude $(shell pkg-config --cflags Qt6Widgets Qt6Concurrent)
CXXFLAGS += -O2 -fPIC -std=c++20 -Wall -Wextra
LDLIBS += $(shell pkg-config --libs Qt6Widgets Qt6Concurrent) -Llib -l:libcapstone.so.5
LDFLAGS += -Wl,-rpath,'$$ORIGIN/lib'
BUILD_VERSION = -DMOST_BUILD_VERSION='"$(shell date -u +%Y%m%d.%H%M%SZ)"'
BUILD_INPUTS := Makefile $(wildcard src/*.hpp runtime64/*) lib/libcapstone.so.5

most: src/main.cpp src/gui.cpp src/engine.cpp src/win64.cpp src/pe.cpp $(BUILD_INPUTS)
	$(CXX) $(CPPFLAGS) $(BUILD_VERSION) $(CXXFLAGS) $(LDFLAGS) $(filter src/%.cpp,$^) $(LDLIBS) -o $@.tmp
	mv -f $@.tmp $@

gui-smoke: tests/gui_smoke.cpp src/gui.cpp src/engine.cpp src/win64.cpp src/pe.cpp $(BUILD_INPUTS)
	$(CXX) $(CPPFLAGS) $(BUILD_VERSION) $(CXXFLAGS) $(LDFLAGS) $(filter-out src/gui.cpp,$(filter src/%.cpp tests/%.cpp,$^)) $(LDLIBS) -o $@.tmp
	mv -f $@.tmp $@
