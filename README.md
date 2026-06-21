# cfrd — CFR Downloader

Builds a single HTML reference document of FAA 14 CFR sections curated for aviators, sourced from the eCFR API. The default section set follows the ASA FAR/AIM and can be customized via `cfr-parts.yaml`.

## Dependencies

Install dependencies on a Debian system with the command below. All other dependencies (fmt, yaml-cpp, cpp-httplib, CLI11) are fetched and built automatically by CMake via FetchContent. Alternatively, use [Docker](#docker) (see below).

```bash
sudo apt install build-essential cmake libssl-dev ninja-build
```

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The binary `build/cfrd` is the only build artifact.

## Usage

```
cfrd <parts.yaml> [-o|--output <dir>] [--test|-t] [--version]
```

| Argument | Description |
|---|---|
| `parts.yaml` | Required. Path to the curated CFR parts config (e.g. `cfr-parts.yaml`) |
| `-o, --output <dir>` | Output directory for the combined HTML file (default: current directory) |
| `-t, --test` | Limit to first 5 API calls — useful for verifying connectivity |
| `--version` | Print version and exit |

### Examples

```bash
# Full download — outputs cfr-parts.html in the current directory
./build/cfrd cfr-parts.yaml

# Write output to a specific directory
./build/cfrd cfr-parts.yaml --output ~/Documents/cfr

# Test run — downloads first 5 sections only
./build/cfrd cfr-parts.yaml --test
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

### Build the image

```bash
docker compose build
```

This produces a local Docker image containing the `cfrd` binary. The binary is not accessible directly on the host — it runs inside the container and writes output to the host only via the volume mount.

### Run

The `output/` directory must exist before running — if Docker creates it automatically it will be root-owned and the container cannot write to it.

```bash
mkdir -p output

# Full download — output written to ./output/cfr-parts.html
docker compose run --rm cfrd cfr-parts.yaml --output /data/output

# Test run
docker compose run --rm cfrd cfr-parts.yaml --output /data/output --test
```

The `--rm` flag removes the container after each run. The `cfr-parts.yaml` file is mounted read-only from the project directory. Output is written to `./output/` on the host via the volume mount.

---

## Editor LSP Setup (clangd)

CMake generates `compile_commands.json` in the `build/` directory. The `.clangd` file is configured to read from this path.
