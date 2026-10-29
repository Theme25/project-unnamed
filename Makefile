# Strict IEEE-754 semantics are required for the accepted Flash port.
CXX ?= g++
CXXFLAGS ?= -O2 -g
override CXXFLAGS += -pthread -std=c++17 -Wall -Wextra -ffp-contract=off -fno-fast-math -fexcess-precision=standard
WINFLAGS = -D__USE_MINGW_ANSI_STDIO=1 -static
ifeq ($(OS),Windows_NT)
  EXE := .exe
  override CXXFLAGS += $(WINFLAGS)
  MINGW_CXX ?= $(CXX)
  MINGW_WINDRES ?= windres
else
  MINGW_CXX ?= x86_64-w64-mingw32-g++-posix
  MINGW_WINDRES ?= x86_64-w64-mingw32-windres
endif
PYTHON ?= python3
SIM_SRC := src/libm_intel.S src/libm_x64_ocx.S src/libm_x64_fp30.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp src/flashpix.cpp
SRC := $(SIM_SRC) src/rbsim.cpp src/verify.cpp src/calib.cpp src/snapshot.cpp src/smartforce.cpp
HDR := $(wildcard src/*.h src/*.inc)
VIEW_SRC := viewer/rbview.cpp viewer/tas.cpp viewer/smartforce_config.cpp viewer/smartforce_ui.cpp
VIEW_HDR := $(wildcard viewer/*.h)

all: rbsim$(EXE)

rbsim$(EXE): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

windows: rbsim.exe
ifneq ($(OS),Windows_NT)
rbsim.exe: $(SRC) $(HDR)
	$(MINGW_CXX) $(CXXFLAGS) $(WINFLAGS) -o $@ $(SRC)
endif

viewer: rbview.exe
viewer/rbview.res.o: viewer/rbview.rc viewer/rbview.ico viewer/rbview.manifest docs/smartforce-string-guide.txt
	cd viewer && $(MINGW_WINDRES) -O coff -o rbview.res.o rbview.rc

rbview.exe: $(VIEW_SRC) $(VIEW_HDR) viewer/rbview.res.o $(SRC) $(HDR)
	$(MINGW_CXX) $(CXXFLAGS) $(WINFLAGS) -DRBSIM_EMBEDDED -municode -mwindows -o $@ $(VIEW_SRC) $(SRC) viewer/rbview.res.o \
		-lgdiplus -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -lole32 -lshell32

test: rbsim$(EXE)
	./rbsim$(EXE) test

tastest: viewer/tas_test$(EXE)
viewer/tas_test$(EXE): viewer/tas_test.cpp viewer/tas.cpp viewer/tas.h $(SIM_SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ viewer/tas_test.cpp viewer/tas.cpp $(SIM_SRC)

deathcause: tools/deathcause$(EXE)
tools/deathcause$(EXE): tools/deathcause.cpp $(SIM_SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ tools/deathcause.cpp $(SIM_SRC)

framescan: tools/framescan$(EXE)
tools/framescan$(EXE): tools/framescan.cpp $(SIM_SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ tools/framescan.cpp $(SIM_SRC)

tools/trig_flip_search$(EXE): tools/trig_flip_search.cpp $(SIM_SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tools/trig_flip_search.cpp $(SIM_SRC)

# Regeneration requires the original extraction inputs; normal builds do not.
libm:
	$(PYTHON) tools/hotspot2gas.py $(OPENJDK) > src/libm_intel.S

levels:
	$(PYTHON) tools/extract_levels.py $(SWFXML) > levels.json
	$(PYTHON) tools/gen_levels_data.py levels.json > src/levels_data.h

clean:
	rm -f rbsim rbsim.exe rbview.exe viewer/rbview.res.o viewer/tas_test viewer/tas_test.exe \
		tools/deathcause tools/deathcause.exe tools/framescan tools/framescan.exe tools/trig_flip_search tools/trig_flip_search.exe

.PHONY: all windows viewer test tastest deathcause framescan libm levels clean
