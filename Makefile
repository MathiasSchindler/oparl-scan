CC := clang
NEWOS := vendor/newos
HOST_OS := $(shell uname -s)
HOST_ARCH := $(shell uname -m)
TARGET ?= $(if $(filter Darwin,$(HOST_OS)),macos,linux)
ARCH ?= $(if $(filter arm64 aarch64,$(HOST_ARCH)),aarch64,x86_64)
BUILD := build/$(TARGET)-$(ARCH)
EXE := $(if $(filter windows,$(TARGET)),.exe,)
PROGRAM := $(BUILD)/oparl-scan$(EXE)
INDEX_PROGRAM := $(BUILD)/oparl-index$(EXE)
SEARCH_PROGRAM := $(BUILD)/oparl-search$(EXE)
PREPARE_PROGRAM := $(BUILD)/oparl-prepare$(EXE)

ifeq ($(wildcard $(NEWOS)/Makefile),)
$(error newos submodule is missing; run git submodule update --init --recursive)
endif

RUNTIME := $(filter-out src/platform/%,$(shell sed -n 's/^MACOS_FREESTANDING_RUNTIME_SOURCES := //p' $(NEWOS)/Makefile))
CRYPTO := $(sort $(shell grep -oE '"src/shared/crypto/[^"]+\.c"' $(NEWOS)/src/compiler/source_manifest.h | tr -d '"'))
TLS := $(sort $(shell grep -oE '"src/shared/tls/[^"]+\.c"' $(NEWOS)/src/compiler/source_manifest.h | tr -d '"'))
COMMON := $(addprefix $(NEWOS)/,$(sort $(RUNTIME) $(CRYPTO) $(TLS) src/shared/hash_util.c))
INDEX_COMMON := $(addprefix $(BUILD)/obj/$(NEWOS)/src/shared/,pdf.o pdf_writer.o compression/zlib.o compression/crc32.o)
CPPFLAGS := -I$(NEWOS)/src/shared -Isrc -DNEWOS_HAVE_PTHREAD=0 \
	-DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -DNEWOS_RUNTIME_ALLOC_LOCK=3
CFLAGS := -std=c11 -Oz -Wall -Wextra -Wpedantic -ffreestanding -fno-builtin \
	-fno-strict-aliasing \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections
APP := src/oparl-scan.c src/http.c src/archive-version.c src/archive-walk.c

ifeq ($(TARGET),macos)
ifneq ($(ARCH),aarch64)
$(error The pinned newos no-import Mach-O backend supports macOS/aarch64)
endif
TRIPLE := arm64-apple-macos11
SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path)
CPPFLAGS += -DNEWOS_MACOS_NEWLINKER -DNEWOS_CRYPTO_CURVE25519_FORCE_64BIT=1
CFLAGS += -isysroot $(SDKROOT) -fno-common -fno-jump-tables -flto
# Apple Silicon guarantees SHA-256 instructions; generic ARM64 targets do not.
$(BUILD)/obj/$(NEWOS)/src/shared/crypto/sha256.o: CPPFLAGS += -DNEWOS_CRYPTO_SHA256_ENABLE_ARM_SHA=1
$(BUILD)/obj/$(NEWOS)/src/shared/crypto/sha256.o: CFLAGS += -O3 -march=armv8-a+crypto
PLATFORM := $(NEWOS)/src/platform/macos/freestanding.c $(NEWOS)/src/platform/macos/newlinker_runtime.c
START := $(NEWOS)/src/platform/macos/newlinker_start.S
NEWOS_HOST := $(if $(filter Darwin,$(HOST_OS)),macos,linux)-$(if $(filter arm64 aarch64,$(HOST_ARCH)),aarch64,x86_64)
LINKER := $(NEWOS)/build/host-$(NEWOS_HOST)/linker
LINK_DEPS := $(LINKER)
define link
$(LINKER) --target=mach-o-arm64 --macho-compact --gc-sections --lto-cc="$(CC)" -o $@.part $(1)
mv -f $@.part $@
endef
else ifeq ($(TARGET),linux)
TRIPLE := $(ARCH)-unknown-linux-elf
CPPFLAGS += -I$(NEWOS)/src/platform/linux -I$(NEWOS)/src/platform/common -I$(NEWOS)/src/arch/$(ARCH)/linux -DNEWOS_DISABLE_STACK_GUARD_INIT=1
PLATFORM := $(addprefix $(NEWOS)/src/platform/linux/,fs.c identity.c io.c net.c process.c thread.c time.c stack_guard.c)
START := $(NEWOS)/src/arch/$(ARCH)/linux/crt0.S $(wildcard $(NEWOS)/src/arch/$(ARCH)/linux/syscall_stubs.S)
CFLAGS += -fno-pie
define link
$(CC) --target=$(TRIPLE) -nostdlib -static -fuse-ld=lld -Wl,--gc-sections -Wl,-e,_start -o $@ $(1)
endef
else ifeq ($(TARGET),windows)
TRIPLE := $(ARCH)-w64-windows-gnu
CPPFLAGS += -I$(NEWOS)/src/platform/windows
PLATFORM := $(NEWOS)/src/platform/windows/core.c $(NEWOS)/src/platform/windows/thread.c $(NEWOS)/src/platform/windows/random.c
START := $(NEWOS)/src/arch/$(ARCH)/windows/chkstk.S
IMPORTS := $(addprefix $(BUILD)/imports/lib,kernel32.a ws2_32.a bcrypt.a)
LINK_DEPS := $(IMPORTS)
CFLAGS += -flto
define link
$(CC) --target=$(TRIPLE) -nostdlib -fuse-ld=lld -flto -Wl,-e,mainCRTStartup -Wl,--gc-sections -Wl,--stack,8388608 -o $@ $(1) -L$(BUILD)/imports -lkernel32 -lws2_32 -lbcrypt
endef
else
$(error Unknown TARGET; use macos, linux, or windows)
endif

OBJECTS := $(addprefix $(BUILD)/obj/,$(addsuffix .o,$(basename $(COMMON) $(PLATFORM) $(START))))
APP_OBJECTS := $(addprefix $(BUILD)/obj/,$(APP:.c=.o))
INDEX_OBJECTS := $(addprefix $(BUILD)/obj/src/,oparl-index.o search.o search-model.o search-date.o search-research.o search-preparation.o pdf-text.o pdf-structure.o archive-version.o archive-walk.o prepare.o)
SEARCH_OBJECTS := $(addprefix $(BUILD)/obj/src/,oparl-search.o search.o search-model.o search-date.o search-research.o search-preparation.o pdf-structure.o search-web.o search-mcp.o search-pdfs.o search-preview-assets.o archive-walk.o)
PREPARE_OBJECTS := $(addprefix $(BUILD)/obj/src/,oparl-prepare.o prepare.o pdf-text.o pdf-structure.o archive-walk.o)
EMBEDDING := src/embedding
EMBEDDING_CORE := $(addprefix $(BUILD)/obj/$(EMBEDDING)/,bundle.o tokenizer.o model.o kernels.o sme.o vectors.o index_io.o)
EMBEDDING_INDEX := $(addprefix $(BUILD)/obj/$(EMBEDDING)/,search.o sections.o content_search.o index.o section_index.o)
$(EMBEDDING_CORE) $(EMBEDDING_INDEX) $(BUILD)/obj/tests/test-embedding.o: CFLAGS += -O3 -ffp-contract=off
$(EMBEDDING_CORE) $(EMBEDDING_INDEX) $(BUILD)/obj/tests/test-embedding.o: CPPFLAGS += -I$(EMBEDDING)
$(BUILD)/obj/$(EMBEDDING)/index.o: CPPFLAGS += -Dmain=eg_title_index_main
$(BUILD)/obj/$(EMBEDDING)/section_index.o: CPPFLAGS += -Dmain=eg_section_index_main
$(BUILD)/obj/src/search-semantic.o: CFLAGS += -O3 -ffp-contract=off
$(BUILD)/test-embedding$(EXE): $(BUILD)/obj/tests/test-embedding.o $(EMBEDDING_CORE) $(BUILD)/obj/$(EMBEDDING)/search.o $(OBJECTS) $(LINK_DEPS)
	$(call link,$(BUILD)/obj/tests/test-embedding.o $(EMBEDDING_CORE) $(BUILD)/obj/$(EMBEDDING)/search.o $(OBJECTS))
PREPARE_SOURCES := src/prepare.c src/prepare.h src/pdf-text.c src/pdf-text.h src/pdf-structure.c src/pdf-structure.h \
	$(NEWOS)/src/shared/pdf.c $(NEWOS)/src/shared/compression/zlib.c \
	$(NEWOS)/src/shared/compression/crc32.c \
	$(wildcard $(NEWOS)/src/shared/*.h $(NEWOS)/src/shared/compression/*.h) $(COMMON) Makefile
PREPARE_ENGINE_ID := $(shell git hash-object $(PREPARE_SOURCES) | git hash-object --stdin)
$(BUILD)/obj/src/prepare.o: CPPFLAGS += -DPREPARE_ENGINE_ID=\"$(PREPARE_ENGINE_ID)\"
$(BUILD)/obj/src/prepare.o: $(PREPARE_SOURCES)

.DEFAULT_GOAL := all
.PHONY: all test test-mcp-acceptance check-deps benchmark
all: $(PROGRAM) $(INDEX_PROGRAM) $(SEARCH_PROGRAM) $(PREPARE_PROGRAM)

$(PROGRAM): $(APP_OBJECTS) $(OBJECTS) $(LINK_DEPS)
	$(call link,$(APP_OBJECTS) $(OBJECTS))

$(INDEX_PROGRAM): $(INDEX_OBJECTS) $(INDEX_COMMON) $(BUILD)/obj/src/search-semantic.o $(EMBEDDING_CORE) $(EMBEDDING_INDEX) $(OBJECTS) $(LINK_DEPS)
	$(call link,$(INDEX_OBJECTS) $(INDEX_COMMON) $(BUILD)/obj/src/search-semantic.o $(EMBEDDING_CORE) $(EMBEDDING_INDEX) $(OBJECTS))

$(SEARCH_PROGRAM): $(SEARCH_OBJECTS) $(BUILD)/obj/src/search-semantic.o $(EMBEDDING_CORE) $(EMBEDDING_INDEX) $(OBJECTS) $(LINK_DEPS)
	$(call link,$(SEARCH_OBJECTS) $(BUILD)/obj/src/search-semantic.o $(EMBEDDING_CORE) $(EMBEDDING_INDEX) $(OBJECTS))

$(PREPARE_PROGRAM): $(PREPARE_OBJECTS) $(INDEX_COMMON) $(OBJECTS) $(LINK_DEPS)
	$(call link,$(PREPARE_OBJECTS) $(INDEX_COMMON) $(OBJECTS))

$(BUILD)/obj/src/search.o $(BUILD)/obj/src/search-model.o $(BUILD)/obj/src/search-date.o $(BUILD)/obj/src/search-research.o $(BUILD)/obj/src/oparl-index.o $(BUILD)/obj/src/oparl-search.o $(BUILD)/obj/src/search-web.o $(BUILD)/obj/src/search-mcp.o $(BUILD)/obj/src/pdf-text.o $(BUILD)/obj/src/pdf-structure.o: CFLAGS += -O3
$(BUILD)/test-json$(EXE): $(BUILD)/obj/tests/test-json.o $(OBJECTS) $(LINK_DEPS)
	$(call link,$(BUILD)/obj/tests/test-json.o $(OBJECTS))

$(BUILD)/obj/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) --target=$(TRIPLE) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) --target=$(TRIPLE) $(CPPFLAGS) -ffreestanding -c $< -o $@

$(BUILD)/obj/src/search-preview-assets.o: src/pdf-preview.js vendor/pdf-reader/dist/pdf-crumb.min.js

$(NEWOS)/build/host-$(NEWOS_HOST)/linker:
	$(MAKE) -C $(NEWOS) build/host-$(NEWOS_HOST)/linker CC="$(CC)"

$(BUILD)/imports/lib%.a: $(NEWOS)/src/platform/windows/imports/%.def
	@mkdir -p $(dir $@)
	llvm-dlltool -m $(if $(filter aarch64,$(ARCH)),arm64,i386:x86-64) -d $< -l $@

test: all $(BUILD)/test-json$(EXE) $(BUILD)/test-embedding$(EXE) check-deps
	$(BUILD)/test-json$(EXE)
	$(BUILD)/test-embedding$(EXE)
	node tests/integration.mjs "$(abspath $(PROGRAM))"
	node tests/incremental.mjs "$(abspath $(PROGRAM))"
	node tests/archive.mjs "$(abspath $(PROGRAM))" "$(abspath $(INDEX_PROGRAM))" "$(abspath $(SEARCH_PROGRAM))"
	node tests/concurrency.mjs "$(abspath $(PROGRAM))"
	node tests/search.mjs "$(abspath $(INDEX_PROGRAM))" "$(abspath $(SEARCH_PROGRAM))"
	node tests/search-model.mjs "$(abspath $(INDEX_PROGRAM))" "$(abspath $(SEARCH_PROGRAM))"
	node tests/search-semantics.mjs "$(abspath $(INDEX_PROGRAM))" "$(abspath $(SEARCH_PROGRAM))"
	node tests/search-research.mjs "$(abspath $(INDEX_PROGRAM))" "$(abspath $(SEARCH_PROGRAM))"
	node tests/prepare.mjs "$(abspath $(PREPARE_PROGRAM))" "$(abspath $(INDEX_PROGRAM))" "$(abspath $(SEARCH_PROGRAM))"

check-deps: all
	node tests/check-deps.mjs "$(abspath $(PROGRAM))"
	node tests/check-deps.mjs "$(abspath $(INDEX_PROGRAM))"
	node tests/check-deps.mjs "$(abspath $(SEARCH_PROGRAM))"
	node tests/check-deps.mjs "$(abspath $(PREPARE_PROGRAM))"

test-mcp-acceptance: all
	node tests/search-acceptance.mjs "$(abspath $(INDEX_PROGRAM))" "$(abspath $(SEARCH_PROGRAM))" "$(abspath $(PROGRAM))" --report build/acceptance/m0-baseline.json

benchmark: all
	node tests/benchmark-search.mjs "$(abspath $(SEARCH_PROGRAM))" build/search/index.ops

-include $(OBJECTS:.o=.d) $(APP_OBJECTS:.o=.d) $(INDEX_OBJECTS:.o=.d) $(SEARCH_OBJECTS:.o=.d) $(PREPARE_OBJECTS:.o=.d) $(INDEX_COMMON:.o=.d) $(EMBEDDING_CORE:.o=.d) $(EMBEDDING_INDEX:.o=.d) $(BUILD)/obj/src/search-semantic.d $(BUILD)/obj/tests/test-embedding.d $(BUILD)/obj/tests/test-json.d
