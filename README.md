# cfrd — CFR Downloader for Aviators

Builds a single HTML reference document of FAA 14 CFR sections curated for aviators, sourced from the live [eCFR API](https://www.ecfr.gov/). The default section set follows the ASA FAR/AIM and can be customized via `cfr-parts.yaml`.

## Dependencies

Install dependencies on a Debian system with the command below. All other dependencies (fmt, yaml-cpp, cpp-httplib, CLI11) are fetched and built automatically by CMake via FetchContent. Alternatively, use [Docker](#docker) (see below).

```bash
sudo apt install build-essential cmake ninja-build libssl-dev libxml2-dev libxslt1-dev
```

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The binary `build/cfrd` is the only build artifact.

## Usage

```
cfrd <parts.yaml> [-o|--output <dir>] [-t|--test] [--xml] [--styled] [--css <file>] [--version]
```

| Argument | Description |
|---|---|
| `parts.yaml` | Required. Path to the curated CFR parts config (e.g. `cfr-parts.yaml`) |
| `-o, --output <dir>` | Output directory (default: current directory) |
| `-t, --test` | Limit to first 5 API calls — useful for verifying connectivity |
| `--xml` | Download raw XML instead of HTML |
| `--styled` | Apply `ecfr.xsl` and embed CSS for styled HTML output |
| `--css <file>` | CSS file to embed instead of the built-in default (requires `--styled`) |
| `--version` | Print version and exit |

### Output modes

| Flags | Output |
|---|---|
| _(none)_ | HTML via custom renderer with table of contents |
| `--xml` | Raw eCFR XML |
| `--styled` | Styled HTML via `ecfr.xsl` with built-in CSS |
| `--styled --css foo.css` | Styled HTML via `ecfr.xsl` with custom CSS |

### Examples

```bash
# Full download — outputs cfr-parts.html in the current directory
./build/cfrd cfr-parts.yaml

# Styled output with built-in CSS
./build/cfrd cfr-parts.yaml --styled

# Styled output with custom CSS
./build/cfrd cfr-parts.yaml --styled --css my-style.css

# Raw XML download
./build/cfrd cfr-parts.yaml --xml

# Test run — first 5 sections only (uses the minimal test config)
./build/cfrd test/cfr-test.yaml --test

# Test run with styled output and a custom stylesheet
./build/cfrd test/cfr-test.yaml --styled --css test/test.css --test

# Write output to a specific directory
./build/cfrd cfr-parts.yaml --styled --output ~/Documents/cfr
```

## Output

A single HTML file named after the input YAML (e.g. `cfr-parts.html`) containing:

- A table of contents with part headings and section links
- All downloaded CFR sections in FAR order
- Appendices where available

Individual section fragments are staged in a temporary directory under `/tmp/` and removed automatically on successful completion. On abort (Ctrl+C) or error the temp directory is retained for inspection.

## Regulation Date

The `cfr-parts.yaml` file contains a top-level `date` field that controls which version of the regulations is fetched from the eCFR API:

```yaml
date: "2025-06-01"
```

Update this date to retrieve a more recent version of the CFR. The eCFR API accepts dates in `YYYY-MM-DD` format. Note that section numbers can change between versions — if you update the date, re-run with `--test` first to check for any new 404s before a full download.

## Docker

Requires [Docker](https://docs.docker.com/get-docker/) and [Docker Compose](https://docs.docker.com/compose/install/). No other system dependencies are needed.

Docker files live in the `docker/` subdirectory. All commands below are run from the **project root**.

### Build the image

```bash
docker compose -f docker/compose.yml build
```

This produces a local Docker image containing the `cfrd` binary. The binary is not accessible directly on the host — it runs inside the container and writes output to the host only via the volume mount.

### Run

The `output/` directory must exist before running — if Docker creates it automatically it will be root-owned and the container cannot write to it.

```bash
mkdir -p output

# Full download — output written to ./output/cfr-parts.html
docker compose -f docker/compose.yml run --rm cfrd cfr-parts.yaml --output /data/output

# Styled output
docker compose -f docker/compose.yml run --rm cfrd cfr-parts.yaml --styled --output /data/output

# Test run — first 5 sections only
docker compose -f docker/compose.yml run --rm cfrd cfr-parts.yaml --output /data/output --test
```

The `--rm` flag removes the container after each run. `cfr-parts.yaml` is mounted read-only from the project root. Output is written to `./output/` on the host via the volume mount.

---

## Editor LSP Setup (clangd)

CMake generates `compile_commands.json` in the `build/` directory. The `.clangd` file is configured to read from this path.
