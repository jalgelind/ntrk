# Builds and runs ntrk's own tests -- no gn, no Catch2, nothing but a C++11
# compiler. That is the standalone claim third_party/ntrk makes (see its
# README.md and the root Makefile's comment above its own ntrk section); this
# is how the claim gets checked from inside the library itself, not just from
# the root build.
#
# The root Makefile's `test` target delegates to the matching targets here, so
# there is one definition of how this library builds and tests, not two.
#
# `make` or `make test` runs everything; objects and binaries land under out/,
# never beside the sources.

all: test

CXXFLAGS := -std=c++11 -Wall -Wextra -ffp-contract=off
OUT       := out
OBJDIR    := $(OUT)/ntrk

# By wildcard, not by list: a source or header added, renamed or removed here
# is picked up without anyone updating this file. (Same reasoning as the root
# Makefile's NTRK_HDRS.)
HDRS    := $(wildcard *.h)
ALL_CC  := $(wildcard *.cc)
TEST_CC := $(wildcard test_*.cc)
# Every non-test source -- the library files the standalone-TU check below
# compiles alone. Includes tools with a main (ntrk_render.cc, ntrk_gen.cc,
# ntrk_bench.cc): nothing links them, the compile is the check.
LIB_CC  := $(filter-out $(TEST_CC),$(ALL_CC))

.PHONY: all test test-ntrk test-ntrk-fx test-ntrk-fx-tu test-ntrk-hdr \
        test-ntrk-import test-ntrk-mix clean

test: test-ntrk test-ntrk-fx test-ntrk-fx-tu test-ntrk-hdr test-ntrk-import \
      test-ntrk-mix

# ---- the four self-contained test binaries ---------------------------------
# Each pulls the library in through a single #include (ntrk.h, ntrk_unity.h,
# or ntrk_import.cc directly -- see the top of each test_*.cc), so compiling
# just that one file already proves it against nothing but a compiler.

$(OUT)/test_ntrk: test_ntrk.cc $(HDRS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -O1 -o $@ test_ntrk.cc

test-ntrk: $(OUT)/test_ntrk
	@$(OUT)/test_ntrk

$(OUT)/test_ntrk_fx: test_ntrk_fx.cc $(HDRS) $(LIB_CC)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -O2 -I. -o $@ test_ntrk_fx.cc

test-ntrk-fx: $(OUT)/test_ntrk_fx
	@$(OUT)/test_ntrk_fx

$(OUT)/test_ntrk_mix: test_ntrk_mix.cc $(HDRS) $(LIB_CC)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -O2 -I. -o $@ test_ntrk_mix.cc

test-ntrk-mix: $(OUT)/test_ntrk_mix
	@$(OUT)/test_ntrk_mix

# Its strongest check reads any real .mod or .xm under testdata/, addressed by a
# repo-root-relative path baked into test_ntrk_import.cc
# ("third_party/ntrk/..."), so the binary has to be *run* from the repo root
# even though it's built from here.
NTRK_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT := $(abspath $(NTRK_DIR)/../..)

# ntrk_import.cc is a prerequisite because this test includes it directly. It
# was missing, and a change to the importer therefore did not rebuild its own
# test -- which reported a pass from a stale binary for several runs before a
# header change happened to force the rebuild and the failure with it.
$(OUT)/test_ntrk_import: test_ntrk_import.cc ntrk_import.cc $(HDRS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -O2 -I. -o $@ test_ntrk_import.cc

test-ntrk-import: $(OUT)/test_ntrk_import
	@cd $(REPO_ROOT) && $(abspath $(OUT)/test_ntrk_import)

# ---- the standalone-TU and standalone-header checks ------------------------
# See the root Makefile's comment above its own test-ntrk-hdr rule: a header
# that only compiled through one specific insertion point in one other file
# shipped green once, because nothing compiled it alone. These two rules are
# what would have caught it, so both stay -Werror.

TU_OBJS := $(patsubst %.cc,$(OBJDIR)/%.o,$(LIB_CC))

$(OBJDIR)/%.o: %.cc $(HDRS)
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -Werror -O2 -I. -c $< -o $@

test-ntrk-fx-tu: $(TU_OBJS)
	@echo "-> ntrk: every source compiles as a standalone translation unit"

# `-Wno-unused-const-variable` because a header alone has no users by
# construction -- a public constant read only from a .cc is unused here and
# always will be, which says nothing about the header.
HDR_OBJS := $(patsubst %.h,$(OBJDIR)/%.hdr.o,$(HDRS))

$(OBJDIR)/%.hdr.o: %.h
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -Werror -Wno-unused-const-variable -c -x c++ $< -o $@

test-ntrk-hdr: $(HDR_OBJS)
	@echo "-> ntrk: every header compiles on its own"

clean:
	rm -rf $(OUT)
