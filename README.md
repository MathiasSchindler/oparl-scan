# OParl-Scan

OParl-Scan aims to make public council information easier to archive and
analyze through OParl APIs across municipalities. Potsdam is the first
supported endpoint and the initial focus for data analysis:

<https://www.potsdam.sitzung-online.de/oparl/system>

The current tool is a freestanding C downloader. It archives the system,
every published body, and all their organization,
person, meeting, membership, location, legislative-term, agenda-item, paper,
consultation, and file collections. Document binaries can be disabled.
Both bodies are discovered from the API, not hardcoded.

The production host/path allowlist and reference-year convention currently
target Potsdam. Other endpoints need explicit support; setting `--system`
alone does not enable arbitrary production hosts. The project name describes
the broader goal, not a claim that every OParl implementation is supported.

## Dependencies and build

The executable uses **no standard C library, CRT, libcurl, OpenSSL, external
JSON library, subprocesses, or shell commands**. It uses newos's shared runtime,
buffers, Unicode handling, SHA-256, JSON output, TLS and X.509 implementation,
and its operating-system backends. `src/json.c` supplies the reading side:
newos's `src/shared/tool_json.c` writes JSON but does not parse it.

`vendor/newos` is a pinned Git submodule of the public
[newos repository](https://github.com/mathiasschindler/newos), under CC0.
Its sources are fetched separately, not copied into OParl-Scan's Git history.
The submodule records the exact revision used by the build.

Clone the project with its dependency:

```sh
git clone --recurse-submodules https://github.com/MathiasSchindler/oparl-scan.git
cd oparl-scan
```

For an existing clone without submodules, run:

```sh
git submodule update --init --recursive
```

Build-time tools are Clang/LLVM, GNU make, and a POSIX build shell with grep,
sed and mkdir. macOS additionally needs the Apple command-line tools/SDK.
Windows builds need LLVM's lld and llvm-dlltool; Git Bash or MSYS2 can provide
the build shell. None of those build tools is required to run the executable.

```sh
make -j4                                    # native macOS/ARM64 or Linux
make -j4 TARGET=linux ARCH=x86_64
make -j4 TARGET=linux ARCH=aarch64
make -j4 TARGET=windows ARCH=x86_64
make -j4 TARGET=windows ARCH=aarch64
```

Output: `build/<target>-<arch>/oparl-scan`, with `.exe` on Windows.
The pinned newos no-import macOS backend supports Apple Silicon.
The macOS build bootstraps newos's linker as a hosted build-time tool; the
resulting downloader uses newos's startup and Mach-O linker, with no dylib
imports. Linux produces static ELF files without a dynamic loader. Windows
imports only the native KERNEL32, WS2_32, and BCRYPT operating-system DLLs,
not the Microsoft CRT.

Build products in `build/` and local captures in `downloads/` are excluded
from Git. The repository contains code, not downloaded council data.

## Usage

```sh
# Full archive: metadata of both bodies and all public document files.
build/macos-aarch64/oparl-scan --output downloads

# Metadata only.
build/macos-aarch64/oparl-scan --output downloads --metadata-only

# Proceedings originating in 2025, including later consultations and documents.
build/macos-aarch64/oparl-scan --year 2025 --output downloads/2025

# Small live smoke test, deliberately incomplete (exit status 2).
build/macos-aarch64/oparl-scan --output build/live-smoke \
  --page-size 2 --max-pages 1 --max-files 1
```

On Linux or Windows, substitute the corresponding executable path.
Use forward slashes in paths on every platform. Output directory parents must
not be symbolic links, following newos's directory-creation policy.

The default is one sequential request at a time, with a 500 ms pause before
each request and up to three attempts for transient failures. Numeric
`Retry-After` values up to 300 seconds are respected; other values stop with
an explicit error. HTTP redirects are restricted to the public API and
`egov.potsdam.de/public/`. No credentials or non-public areas are used.

### Selecting a reference year

`--year 2025` means references starting with **`25/`**, including `25/SVV/...`,
`25/OBR/...` and suffixes such as `25/SVV/0403-001`. It does **not** filter the
Paper's `date`, meeting dates, or document timestamps. For example, the real
`25/SVV/1216` currently has a Paper date in March 2026 and is still included.
Supported reference years are 2000 through 2099.

The program first scans the complete Paper lists of both bodies. No verified
server-side reference filter is available: the tested `paperNumber` parameter
is ignored by this API. This metadata-only discovery scan is necessary to avoid
missing late-created, renumbered, or out-of-order records. Its unfiltered raw
responses are stored separately under `index/`.

Matching Papers are retrieved individually. Their main files, attachments,
parent/child and related Papers, consultations, specific agenda items,
resolution files, and linked meetings are followed recursively, with identity
deduplication to stop cycles. This includes linked proceedings and documents
from later years. The API's nonstandard lowercase `agendaitem` field is
supported alongside `agendaItem`.

For linked meetings, the archive includes invitations, protocols, attachments,
and organizational/location context. The raw Meeting JSON can contain the
whole agenda, and meeting-wide PDFs can cover other proceedings. However,
unrelated agenda items are not recursively expanded or downloaded individually:
otherwise one selected proceeding would eventually pull in the entire RIS.
Organization and person records provide context without expanding reverse
membership and collection links.

`selection.jsonl` lists the matching seed Paper IDs and references; linked
follow-up Papers may have other reference years. `archive.json` records
`referenceYear`, `scannedPapers`, `selectedPapers`, and `processedResources`,
and is updated during the run. `selectedPapers` counts roots, not all linked
context objects. Each year/system must use a separate output directory;
attempting to mix them is rejected without replacing the existing identity.

### TLS and trust

HTTPS uses newos's shared TLS 1.3 implementation, with a TLS 1.2 fallback
for the document server. Certificate chain, hostname, and validity checks
are performed explicitly through shared X.509 code on **every platform**.
There is no insecure/skip-verification option.

The trust bundle is read from `--ca-file FILE`, then `SSL_CERT_FILE`, or the
first available standard Unix PEM bundle. Windows needs a PEM bundle supplied
via the option or environment variable; the Windows certificate store is not
used. A CA bundle is trust data, not a linked library.

`--timeout-ms` defaults to 30000 and bounds TLS operations and response reads,
not the total runtime. DNS resolution and TCP connection establishment use
newos's blocking platform API and are not covered by this timeout.
HTTP without TLS is allowed only for an explicitly selected `127.0.0.1`
test system.

### Archive and resuming

```
downloads/
  archive.json
  selection.jsonl                         # reference-year mode only
  index/<sha256-request-url>.json          # unfiltered discovery pages, year mode only
  metadata/<sha256-request-url>.json
  documents/<sha256-oparl-file-id>-<sanitized-original-filename>
```

Metadata pages retain the original UTF-8 JSON, including relationships,
pagination links, file IDs, original filenames, download URLs, and timestamps.
Hashes are lowercase hex SHA-256 of the UTF-8 URL/ID string, not of file bytes.
Filenames are restricted to ASCII letters, numbers, `.`, `_` and `-`, and
the original-name suffix is limited to 100 bytes. IDs prevent collisions
between documents with the same filename. `archive.json` identifies the system
and records completion status and per-run counters.

Downloads are streamed to `.part` files and renamed only after completion.
Metadata is validated before publication, and supplied OParl document sizes
are checked. The same File ID is downloaded only once per run, even if it
appears inline in several collections.

Run the same command again to resume. Valid cached metadata is read locally;
existing documents are checked against their declared size and skipped.
Incomplete `.part` files are restarted, not resumed byte by byte.
Cache reuse is for resuming an archive, **not an incremental synchronization**:
it does not recheck remote modifications or detect same-size local corruption.
Use `--refresh` to replace cached metadata and documents, or use a new output
directory for another capture. Old files are not automatically deleted.
Do not run two processes against the same output directory.

`--max-pages N` limits pages **per collection**, including the bodies list.
`--max-files N` limits **new document downloads per invocation**; cached
documents do not consume this budget. Remove limits to finish the archive.

Exit codes:

- `0`: all discovered collections processed, within the selected mode.
- `1`: explicit network, TLS, JSON, pagination, filesystem, or document error.
- `2`: intentionally incomplete because of configured limits.

In metadata-only mode, completion means metadata completion, not document
completion; this distinction is recorded in `archive.json`.
An interrupted run leaves `complete: false`. OParl is a live API, so an archive
is not an atomic point-in-time database snapshot.

Limits are 32 MiB per metadata page, 4 GiB per document, and JSON nesting
depth 64. Errors stop the run; HTTP failures and invalid input are never
replaced with empty successful responses.

## Tests and verification

Node.js is needed only for tests and executable dependency inspection:

```sh
make test
make check-deps
make TARGET=linux ARCH=x86_64 check-deps
make TARGET=windows ARCH=aarch64 check-deps
```

Parser tests run as a freestanding newos executable. The integration tests
use a temporary local fixture server, with pagination, both bodies, all
collection types, inline duplicate documents, chunked HTTP, redirects,
resuming, metadata-only mode, refresh, explicit limits, malformed responses,
truncation, retries, reference-year selection, later linked proceedings and
documents, graph deduplication, and archive identity checks.

Verified locally on macOS/ARM64: native execution, fixture tests, certificate-
verified live API and PDF downloads, and metadata-cache reuse. Linux and
Windows x86-64/ARM64 builds are cross-compiled and checked for runtime imports;
they have not been executed on those operating systems here. No full live
mass download is started by building or running the test suite.

## License

OParl-Scan is released under [CC0 1.0 Universal](LICENSE).
The separately fetched newos submodule is also under CC0.
