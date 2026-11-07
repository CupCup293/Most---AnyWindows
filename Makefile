CXX ?= g++
CPPFLAGS += -Iinclude $(shell pkg-config --cflags Qt6Widgets Qt6Concurrent)
CXXFLAGS += -O2 -fPIC -std=c++20 -Wall -Wextra
LDLIBS += $(shell pkg-config --libs Qt6Widgets Qt6Concurrent) -Llib -l:libcapstone.so.5
LDFLAGS += -Wl,-rpath,'$$ORIGIN/lib'

most: src/main.cpp src/gui.cpp src/engine.cpp src/win64.cpp src/pe.cpp $(wildcard src/*.hpp) lib/libcapstone.so.5
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(filter %.cpp,$^) $(LDLIBS) -o $@.tmp
	mv -f $@.tmp $@

gui-smoke: tests/gui_smoke.cpp src/gui.cpp src/engine.cpp src/win64.cpp src/pe.cpp $(wildcard src/*.hpp) lib/libcapstone.so.5
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(filter-out src/gui.cpp,$(filter %.cpp,$^)) $(LDLIBS) -o $@.tmp
	mv -f $@.tmp $@
