# Warning: This makefile rampantly assumes it is being run in UNIX
#   Also, there may be some gnu tool chain assumptions: gcc and gmake?

# ***********
# * PREFACE *
# ***********------+
# | Subdirectories |
# +----------------+
# declare subdirectories of the source directory
SUBDIRECTORIES := core headers test

# make sure the subdirectories are mirrored in
# the obj/ and debug/ directories
DIRS := bin lib include obj debug
# use this dummy variable to get access to shell commands
SHELL_HACK := $(shell mkdir -p $(DIRS))

# +----------+
# | Platform |
# +----------+-------------------
# | evaluates to 'Darwin' on mac
# | evaluates to ??? on ???
# +------------------------------
PLATFORM := $(shell uname)

-include makeConstants

# *********
# * TOOLS *
# *********--+
# | Programs |
# +----------+
CXX_STANDARD_FLAGS := -std=c++20
CC  := g++
CXX := g++
ifeq ($(PLATFORM),Darwin) # on mac
    CXX_STANDARD_FLAGS := $(CXX_STANDARD_FLAGS) -stdlib=libc++ -Wno-c++11-extensions
endif
RM  := rm
CP  := cp

# +--------------+
# | Option Flags |
# +--------------+
# Let the preprocessor know which platform we're on
ifeq ($(PLATFORM),Darwin)
  CONFIG  := $(CONFIG) -DMACOSX
else
  CONFIG  := $(CONFIG)
endif

# include paths (flattens the hierarchy for include directives)
INC       := -I.

# use the second line to disable profiling instrumentation
# PROFILING := -pg
PROFILING :=
CCFLAGS   := -Wall $(INC) $(CONFIG) -O2 -DNDEBUG $(PROFILING)
CXXFLAGS  := $(CCFLAGS) $(CXX_STANDARD_FLAGS) -MMD -MP
CCDFLAGS  := $(INC) $(CONFIG) -ggdb
CXXDFLAGS := $(CCDFLAGS) $(CXX_STANDARD_FLAGS) -MMD -MP

LINK         := -Wall $(INC) $(CONFIG) -O2 -DNDEBUG $(PROFILING)
LINKD        := -Wall $(INC) $(CONFIG) -ggdb
ifeq ($(PLATFORM),Darwin)
  LINK  := $(LINK) -Wl,-no_pie
  LINKD := $(LINK) -Wl,-no_pie
endif


# ***********************
# * SOURCE DECLARATIONS *
# ***********************-----------------+
# | SRCS defines a generic bag of sources |
# +---------------------------------------+
SRCS         :=     Cell BigCell ProtoList ProtoSparseList ParentLink ProtoTuple ProtoString ProtoByteBuffer 	ProtoContext Proto ProtoExternalPointer ProtoObjectCell 	ProtoMethodCell Thread ProtoSpace

# +-----------------------------------+
# | HEADERS defines headers to export |
# +-----------------------------------+
HEADERS           := proto.h proto_internal.h
HEADER_COPIES     := $(addprefix include/,$(HEADERS))

# +--------------------------------------+
# | Stand alone program sources (Tests)  |
# | Finds all .cpp files in test/ folder |
# +--------------------------------------+
TEST_SRCS = $(wildcard test/*.cpp)
TEST_OBJS_DEBUG = $(patsubst test/%.cpp,obj/test-debug/%.o,$(TEST_SRCS))

PERF_SRCS = $(wildcard performance/*.cpp)
PERF_OBJS_DEBUG = $(patsubst performance/%.cpp,obj/performance-debug/%.o,$(PERF_SRCS))

# +--------------------------------+
# | Object Aggregation for Targets |
# +--------------------------------+
FULL_SRC		  := $(addprefix core/,$(addsuffix .cpp,$(SRCS)))
OBJ               := $(addprefix obj/,$(addsuffix .o,$(SRCS)))
DEBUG             := $(addprefix debug/,$(addsuffix .o,$(SRCS)))
TEST_OBJS_DEBUG   := $(patsubst test/%.cpp,debug/%.o,$(TEST_SRCS))

LIB_TARGET_NAME   := proto

# *********
# * RULES *
# *********------+
# | Target Rules |
# +--------------+
.PHONY: all debug test includes clean

all: lib/lib$(LIB_TARGET_NAME).a includes

debug: lib/lib$(LIB_TARGET_NAME)-debug.a bin/test_proto includes

lib/lib$(LIB_TARGET_NAME).a: $(OBJ)
	@echo "Bundling $@"
	@ar rcs $@ $(OBJ)

lib/lib$(LIB_TARGET_NAME)-debug.a: $(DEBUG)
	@echo "Bundling debug $@"
	@ar rcs $@ $(DEBUG)

# --- Test Suite Compilation and Linking ---
bin/test_proto: $(TEST_OBJS_DEBUG) lib/lib$(LIB_TARGET_NAME)-debug.a
	@echo "Linking test_proto command line tool"
	@$(CXX) -o $@ -pthread $(LINKD) $(TEST_OBJS_DEBUG) lib/lib$(LIB_TARGET_NAME)-debug.a

test: bin/test_proto
	@echo "Running test suite"
	@./bin/test_proto

# --- Performance Benchmarks ---
# Generic rule for linking any benchmark executable
bin/%: obj/performance-debug/%.o lib/libproto-debug.a
	@echo "Linking performance benchmark $@"
	@mkdir -p bin
	@$(CXX) -o $@ -pthread $^ lib/lib$(LIB_TARGET_NAME)-debug.a

performance: bin/list_benchmark bin/sparse_list_benchmark bin/object_access_benchmark
	@echo "Running list benchmark ..."
	@./bin/list_benchmark
	@echo "Running sparse list benchmark ..."
	@./bin/sparse_list_benchmark
	@echo "Running object access benchmark ..."
	@./bin/object_access_benchmark

# +------------------------------------+
# | Generic Source->Object Build Rules |
# +------------------------------------+
obj/%.o: core/%.cpp
	@echo "Compiling $@"
	@$(CXX) $(CXXFLAGS) -o $@ -c $<

debug/%.o: core/%.cpp
	@echo "Compiling for debug $@"
	@$(CXX) $(CXXDFLAGS) -o $@ -c $<

# Generic rule for compiling test files into the debug/ directory
debug/%.o: test/%.cpp
	@echo "Compiling test file for debug $@"
	@$(CXX) $(CXXDFLAGS) -o $@ -c $<

obj/performance-debug/%.o: performance/%.cpp
	@echo "Compiling performance benchmark for debug $@"
	@mkdir -p obj/performance-debug
	@$(CXX) $(CXXDFLAGS) -o $@ -c $<


# +-------------------+
# | include copy rule |
# +-------------------+
includes: $(HEADER_COPIES)

include/%.h: headers/%.h
	@echo "updating header $@"
	@cp $< $@

-include $(OBJ:.o=.d) $(DEBUG:.o=.d) $(TEST_OBJS_DEBUG:.o=.d)

# +---------------+
# | cleaning rule |
# +---------------+
clean:
	@echo "--- Limpiando artefactos de compilacion ---"
	-@$(RM) -fv $(DEBUG)
	-@$(RM) -fv $(OBJ)
	-@$(RM) -fv $(TEST_OBJS_DEBUG)
	-@$(RM) -fv $(PERF_OBJS_DEBUG)
	-@$(RM) -fv $(DEBUG:.o=.d) $(OBJ:.o=.d) $(TEST_OBJS_DEBUG:.o=.d)
	-@$(RM) -rfv bin lib obj debug include
	@echo "--- Limpieza finalizada ---"
