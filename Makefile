CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2

.PHONY: all clean

all: build/termess-server

build/termess-server: src/server.cpp src/protocol.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

build:
	mkdir -p $@

clean:
	rm -rf build
