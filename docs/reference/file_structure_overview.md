# File Structure Overview

```
Eldra_V2/
|-- .devcontainer/           # Devcontainer setup for ESP-IDF
|-- .github/                 # PR templates, CI configs
|-- build/                   # Build outputs (generated)
|-- docs/
|   |-- hardware/            # Board, IMU, power, servo notes
|   |-- firmware/            # (reserved) firmware architecture/design docs
|   |-- reference/           # Pin maps, palettes, conversions, file layout
|   `-- roadmap.md           # Project phases and TODOs
|-- managed_components/      # Third-party deps (e.g., LVGL) fetched by IDF component manager
|-- main/                    # Application entry point
|-- components/              # Modular components (display, eyes, sensors, mood, motion)
|-- sdkconfig.defaults       # ESP-IDF defaults
|-- partitions.csv           # Partition table
`-- README.md                # Top-level project overview
```

Notes:
- `managed_components/` is populated automatically from `idf_component.yml` (keep third-party libraries here).
- `vendor_demo/` (removed) was a temporary import of vendor sample code; required drivers have been mirrored into `components/`.
- Use this file to keep directory purposes in sync with the README and CONTRIBUTING guides.
