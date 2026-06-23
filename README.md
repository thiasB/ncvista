# ncvista

A modern, ncview-like visual browser for netCDF files, written in C++ for the
X Window System. It renders with Cairo/Pango for anti-aliased text and smooth,
perceptually-uniform colormaps, reads data through the netCDF C library, and
uses **udunits2** for unit formatting and calendar/time-axis decoding.

It is a self-contained native X11 application — it relies only on the X server
(Xlib), with no GTK/Qt/Motif toolkit.

![ncvista showing the bundled sample.nc — surface temperature with the
coastline overlay](docs/screenshot.png)

## Features

- Sidebar listing every displayable (≥1-D numeric) variable with its long name
  and units. Supporting variables (cell bounds such as `time_bnds`, auxiliary
  coordinates, grid mappings, and coordinate variables such as `depth`/`lev`)
  are detected, dimmed, and sorted to the end; the initial selection is always a
  real data field. The list scrolls (mouse wheel or a draggable scrollbar) and a
  hover tooltip shows the full name and units when an entry is truncated.
- 2-D field rendering of the last two dimensions, aspect-ratio preserved.
- 1-D variables are shown as a line plot filling the main window (the same chart
  as the time-series window), with a calendar/numeric x-axis.
- **Zoom & pan**: drag a rectangle over the plot to zoom into that region (the
  coastline overlay and hover readout follow); right-click to reset. When zoomed,
  pan with the scrollbars along the plot edges, or with the mouse wheel (vertical,
  Shift+wheel for horizontal). The zoom persists across animation frames.
- Sliders for every extra dimension (e.g. level); the time axis is detected via
  udunits and labelled with calendar dates instead of raw numbers. Playback
  controls (first / previous / play / next) sit to the left of the time slider.
- Animation along the time (or first extra) dimension: play/pause, step, speed.
- Built-in colormaps: sequential perceptual (viridis, inferno, plasma, magma,
  cividis, thermal, ocean), white/light-centred diverging ones for anomalies
  (coolwarm, RdYlBu, BrBG, PuOr, seismic), and grayscale. Chosen from a
  dropdown with gradient swatches (the colormap button), or cycled with `c`; the
  **reverse** button above the colour scale flips any palette's direction.
- Colorbar with tick labels. The value range starts **fixed**, derived from a
  quick global min/max scan of the variable (sub-sampled for large files) so the
  colour scale is stable across time steps. The min and max are editable: click
  a colorbar bound field and type a new value (Enter applies, Esc cancels). The
  **fixed** button above the colour scale toggles between this fixed global
  range and per-slice auto. The **symmetric** button (below it) makes the scale
  symmetric around zero — `[-M, +M]` with `M = max(|min|, |max|)` — which centres
  diverging fields (e.g. anomalies) on zero.
- Missing/fill values are rendered white.
- The window opens at a default size whose plot canvas matches a global
  longitude/latitude grid (360 × 180, a 2:1 map); it is freely resizable.
- **Coastlines overlay** (Natural Earth 110m) for longitude/latitude fields,
  auto-detected from coordinate units and enabled by default; handles 0–360°
  and −180–180° longitude conventions. Toggle with the coast button or `l`.
- **Country-borders overlay** (Natural Earth 110m), drawn as dashed lines for
  longitude/latitude fields; off by default. Toggle with the borders button or
  `b`.
- **Map projections** for longitude/latitude fields: equirectangular (default),
  Mollweide, Hammer and Gall-Peters (equal-area), Mercator, and Robinson. Cycle
  with the projection button or `p`; the field is reprojected and the
  coast/border overlays follow. Zoom/hover apply in the equirectangular view.
- Pointer hover readout: data value (with its unit) and the coordinate-variable
  position, plus a crosshair.
- **Time-series window**: click a grid cell in the plot to open a line chart of
  that cell's series along the time axis, at the currently selected vertical
  level (and other fixed dimensions). The x-axis uses udunits calendar dates, the
  current frame is highlighted, and the unit is shown in the title. For large
  (high-resolution / vertically resolved) variables the extraction can be slow, so
  a "please wait" popup is shown while it runs.
- **Metadata window**: a second, scrollable window listing the file's
  dimensions, global attributes, and every variable with its full attribute set
  (an ncdump-style header), colour-coded and monospaced. The text is selectable
  with the mouse and copied to the PRIMARY and CLIPBOARD selections (middle-click
  paste or Ctrl+V into other applications).
- Uniformly sized toolbar buttons, each with a hover tooltip describing its
  action. The toolbar file name is ellipsized when it does not fit; hovering over
  a truncated name shows the complete file name (without the folder path).
- Units are shown verbatim as written in the file's `units` attribute (factor
  order preserved, e.g. `kg m-2`). udunits is still used for time-axis decoding
  for standard/gregorian/proleptic calendars, with arithmetic fallbacks for the
  CF `360_day`, `noleap`/`365_day` and `all_leap`/`366_day` calendars.
- A terminal notice is printed while the global min/max range of a large variable
  is being scanned (this runs before the window is mapped at start-up).
- CF packing is unpacked on read: stored values are converted to physical units
  via `scale_factor` / `add_offset`, so the field, the value range and the colour
  scale are all in physical units (fill values are detected before unpacking).
- `_FillValue` / `missing_value` are rendered white.

## Keyboard / mouse

| Action            | Key / mouse                              |
|-------------------|------------------------------------------|
| Play / pause      | `Space` or the ▶ button                  |
| First time step   | `Home` or the ⏮ button                   |
| Step time         | `←` / `→` or the ◀ / ▶ buttons           |
| Change variable   | `↑` / `↓` or click in the sidebar        |
| Scroll variable list | wheel over the sidebar or drag its scrollbar |
| Choose colormap   | the colormap button (dropdown); `c` cycles |
| Flip Y (N↕S)      | `f` or the flip button                   |
| Auto/fixed range  | `a` or the **fixed** button (above the colour scale) |
| Symmetric scale   | `s` or the **symmetric** button               |
| Reverse colours   | `r` or the **reverse** button            |
| Edit min / max    | click a colorbar bound field, type, Enter|
| Coastlines overlay| `l` or the coast button (geographic data)|
| Country borders   | `b` or the borders button (geographic data)|
| Map projection    | `p` or the projection button (geographic data)|
| Metadata window   | `m` / `i` or the ⓘ metadata button       |
| Select metadata text | drag in the metadata window (copies to clipboard) |
| Long attributes   | soft-wrap to the window width; resize to reflow |
| Animation speed   | `+` / `-` or scroll wheel                |
| Move a dimension  | drag the slider                          |
| Inspect a cell    | hover the plot                           |
| Cell time series  | click a cell in the plot                 |
| Zoom into region  | drag a rectangle over the plot           |
| Pan (when zoomed) | wheel / Shift+wheel, or drag the plot scrollbars |
| Reset zoom        | right-click the plot                     |
| Quit              | `q` or `Esc`                             |

## Build

### Required development libraries

A C++17 compiler, CMake ≥ 3.16, and `pkg-config` are needed, plus the
development (`-dev` / `-devel`) packages for the libraries below. The GUI
libraries are located through `pkg-config`; netCDF and udunits2 are linked
directly.

| Library             | `pkg-config` module | Purpose                                  |
|---------------------|---------------------|------------------------------------------|
| X11 (Xlib)          | `x11`               | native window + event loop               |
| Cairo               | `cairo`             | 2-D rendering                            |
| Cairo XLib surface  | `cairo-xlib`        | drawing onto the X window                |
| Pango + Cairo       | `pangocairo`        | anti-aliased text layout                 |
| libxml2             | `libxml-2.0`        | fontconfig dependency (resolved explicitly) |
| netCDF C library    | (linked as `netcdf`)| reading netCDF files                     |
| udunits2            | (linked as `udunits2`) | unit formatting, calendar/time decoding |

Install them with your package manager:

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config \
     libx11-dev libcairo2-dev libpango1.0-dev libxml2-dev \
     libnetcdf-dev libudunits2-dev

# Fedora / RHEL
sudo dnf install gcc-c++ cmake pkgconf-pkg-config \
     libX11-devel cairo-devel pango-devel libxml2-devel \
     netcdf-devel udunits2-devel

# macOS (Homebrew; X11 via XQuartz)
brew install cmake pkg-config cairo pango libxml2 netcdf udunits
```

(`libcairo2-dev` / `cairo-devel` provides both the `cairo` and `cairo-xlib`
modules; `libpango1.0-dev` / `pango-devel` provides `pangocairo`.)

If netCDF and udunits2 are installed under a non-standard prefix (the common
case on HPC systems), point the build at it with `-DHPC_PREFIX=/path/to/prefix`;
this is the `HPC_PREFIX` cache variable in `CMakeLists.txt`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The runtime library path and udunits XML location are baked into the binary via
rpath / a built-in default, so no environment setup is normally required. If the
udunits database is moved, point to it with:

```sh
export UDUNITS2_XML_PATH="$PREFIX/share/udunits/udunits2.xml"
```

where `$PREFIX` is your netCDF/udunits install prefix.

## Usage

```sh
./build/ncvista FILE.nc
./build/ncvista --version   # print the version and exit
```

A test file generator is included:

```sh
PREFIX=/path/to/prefix     # your netCDF/udunits install prefix
gcc gen_test.c -I"$PREFIX/include" -L"$PREFIX/lib" \
    -Wl,-rpath,"$PREFIX/lib" -lnetcdf -lm -o gen_test
./gen_test                 # writes sample.nc
./build/ncvista sample.nc
```

## Coastline / border data

The overlays read `coastlines.bin` and `borders.bin`, compact little-endian
files of (lon, lat) polylines shipped with the project (Natural Earth 110m
coastline and admin-0 boundary lines, derived from public-domain data). At
runtime each is located via, in order: an environment override (`$NCVISTA_COAST`
/ `$NCVISTA_BORDERS`), next to the executable, `<prefix>/share/ncvista/`, then
the build-time source path.

To regenerate them from Natural Earth shapefiles (the generator uses cartopy or
pyshp if present, otherwise a built-in standard-library reader):

```sh
python3 scripts/make_overlays.py ne_110m_coastline.shp coastlines.bin
python3 scripts/make_overlays.py ne_110m_admin_0_boundary_lines_land.shp borders.bin
```

## Source layout

| File              | Responsibility                                        |
|-------------------|-------------------------------------------------------|
| `src/ncfile.*`    | netCDF wrapper: dims, variables, coords, 2-D slices   |
| `src/units.*`     | udunits2: unit formatting, time-axis decoding         |
| `src/colormap.*`  | built-in perceptual colormaps                         |
| `src/coastline.*` | coastline data loader + overlay path resolution       |
| `src/app.cpp`     | Xlib + Cairo immediate-mode UI and event loop         |

## Notes and limitations

- The two plotted axes are identified by coordinate metadata — the CF `axis`
  attribute (`X`/`Y`), then `standard_name`, then units, then the dimension
  name (all case-insensitive). A longitude/latitude pair is used wherever it
  appears in the dimension list, so permuted orders such as `(lat, lon, time)`
  or `(lon, lat, time)` are handled (the field is transposed to north-up
  row-major as needed). When no geographic pair is recognised, the last two
  non-time dimensions are used as Y, X, so a time axis is never plotted as a
  map axis. Curvilinear/unstructured grids are drawn in index space.
- Time decoding for the standard calendar uses udunits' conversion to a
  reference epoch and POSIX `gmtime` (proleptic Gregorian, no leap seconds),
  which is accurate for typical climate-data date ranges.

## License

Released under the [MIT License](LICENSE).

The bundled `coastlines.bin` and `borders.bin` are derived from
[Natural Earth](https://www.naturalearthdata.com/) vector data, which is in the
public domain. ncvista links the netCDF C library
and udunits2 (BSD-style licenses) and Cairo / Pango (LGPL/MPL); these are not
distributed with the source.
