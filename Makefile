# Strict IEEE-754 double semantics are mandatory for bit-exact Flash parity:
#  -ffp-contract=off   never fuse a*b+c into FMA
#  -fno-fast-math      (default, stated for clarity)
#  x86-64 uses SSE2 scalar doubles (no x87 extended precision)
#
# Windows: build natively with MSYS2's MinGW-w64 GCC (`make` in a "MSYS2 UCRT64" shell), or
# cross-compile from Linux with `make windows` (x86_64-w64-mingw32-g++-posix). Both produce a
# static rbsim.exe that needs no DLLs. MSVC is not supported (the Intel sin/cos is GNU assembly).
CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -pthread -std=c++17 -Wall -Wextra -ffp-contract=off -fno-fast-math -fexcess-precision=standard
WINFLAGS  = -D__USE_MINGW_ANSI_STDIO=1 -static
ifeq ($(OS),Windows_NT)
  EXE      := .exe
  CXXFLAGS += $(WINFLAGS)
endif
SRC = src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp src/rbsim.cpp src/verify.cpp src/calib.cpp src/search.cpp src/snapshot.cpp src/beam.cpp
HDR = src/b2math.h src/b2world.h src/redball.h src/levels_data.h src/display_data.h src/flash_sintab.h src/snapshot.h

rbsim$(EXE): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

# Cross-compile a Windows build from Linux (apt install mingw-w64); test it with wine.
MINGW_CXX ?= x86_64-w64-mingw32-g++-posix
windows: rbsim.exe
rbsim.exe: $(SRC) $(HDR)
	$(MINGW_CXX) -O2 -g $(filter-out -O2 -g,$(CXXFLAGS)) $(WINFLAGS) -o $@ $(SRC)

# Route viewer: one static Windows program (Win32 + GDI+), no console. Same simulator sources, same flags.
MINGW_WINDRES ?= x86_64-w64-mingw32-windres
SIM_SRC := src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp
viewer: rbview.exe
viewer/rbview.res.o: viewer/rbview.rc viewer/rbview.ico viewer/rbview.manifest
	$(MINGW_WINDRES) -O coff -o $@ viewer/rbview.rc
rbview.exe: viewer/rbview.cpp viewer/rbview.res.o $(SIM_SRC) $(HDR)
	$(MINGW_CXX) -O2 $(filter-out -O2 -g,$(CXXFLAGS)) $(WINFLAGS) -municode -mwindows -o $@ viewer/rbview.cpp $(SIM_SRC) viewer/rbview.res.o \
		-lgdiplus -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -lole32

tools/trig_flip_search: tools/trig_flip_search.cpp src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tools/trig_flip_search.cpp src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp

test: rbsim$(EXE)
	./rbsim$(EXE) test

clean:
	rm -f rbsim rbsim.exe tools/trig_flip_search
# Regenerate src/libm_intel.S from OpenJDK sources (needs network):
#   make libm OPENJDK=/path/to/dir-with-stubGenerator_x86_64_{sin,cos,constants}.cpp
libm:
	python3 tools/hotspot2gas.py $(OPENJDK) > src/libm_intel.S

# Regenerate src/levels_data.h from the SWF (needs JPEXS FFDec: -swf2xml):
#   make levels SWFXML=practice_full.xml
levels:
	python3 tools/extract_levels.py $(SWFXML) > levels.json
	python3 tools/gen_levels_data.py levels.json > src/levels_data.h

.PHONY: test clean libm levels windows deathcause viewer

deathcause: tools/deathcause.cpp src/*.cpp src/*.h src/libm_intel.S
	$(CXX) $(CXXFLAGS) -o tools/deathcause tools/deathcause.cpp src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp
