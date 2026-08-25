CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
LDFLAGS ?=
LDLIBS = /usr/lib/x86_64-linux-gnu/libproj.so.22 /usr/lib/x86_64-linux-gnu/libdeflate.so.0 /usr/lib/x86_64-linux-gnu/libzstd.so.1 /usr/lib/x86_64-linux-gnu/libwebp.so.7 -lz -lsqlite3 -ldl -lpthread

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))
GEN  := build/embedded_data.o

executable: $(OBJS) $(GEN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(GEN) $(LDLIBS)

build/%.o: src/%.cpp $(wildcard src/*.h) | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/embedded.S build/embedded_index.cpp: tools/embed.py $(shell find spec -type f) | build
	python3 tools/embed.py

build/embedded_data.o: build/embedded.S build/embedded_index.cpp
	$(CXX) $(CXXFLAGS) -c -o build/embedded_index.o build/embedded_index.cpp
	$(CXX) -c -o build/embedded_incbin.o build/embedded.S
	ld -r -o $@ build/embedded_index.o build/embedded_incbin.o

build:
	mkdir -p build

clean:
	rm -rf build executable

.PHONY: clean
