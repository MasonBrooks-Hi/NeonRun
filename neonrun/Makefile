# NEON RUN - 3D endless runner on creaddinscart.studio.cpp.sl / v3D-1.0.0
#
# The library is header-only, so there is nothing to build or link except the
# game itself. Point V3D at wherever you unpacked it.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra
V3D      ?= $(HOME)/.local
INCLUDE  := -I$(V3D)/include
LDLIBS   := -lz

BIN := neonrun
SRC := src/neonrun.cpp

.PHONY: all clean run gif frames info

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDE) $< -o $@ $(LDLIBS)

run: $(BIN)
	./$(BIN)

# 240 autopilot frames at 640x360, then stitched into a GIF by Python/Pillow
frames: $(BIN)
	@mkdir -p frames
	./$(BIN) --width 640 --height 360 --replay 240 frames

gif: frames
	python3 tools/make_gif.py frames neonrun.gif

info: $(BIN)
	./$(BIN) --info

clean:
	rm -f $(BIN) neonrun.gif
	@echo "generated frames live in ./frames - delete that folder yourself if you want"
