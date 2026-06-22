// ncvista — a modern, ncview-like netCDF viewer.
//
// Rendering: Xlib window with a Cairo (cairo-xlib) surface and Pango text.
// Data:      netCDF C library.
// Units:     udunits2 (unit formatting + calendar/time decoding).
//
// Immediate-mode UI: the whole window is redrawn each frame and pointer events
// are resolved by hit-testing the layout rectangles computed during layout().

#include "ncfile.hpp"
#include "units.hpp"
#include "colormap.hpp"
#include "overlay.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/select.h>
#include <sys/time.h>
#include <vector>

// Build-time version, normally injected by CMake (project VERSION).
#ifndef NCVISTA_VERSION
#define NCVISTA_VERSION "0.0.0-dev"
#endif

namespace {

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool hit(double px, double py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

struct RGB { double r, g, b; };

// Dark, modern palette.
const RGB COL_BG        = {0.114, 0.125, 0.149};
const RGB COL_PANEL     = {0.157, 0.173, 0.204};
const RGB COL_PANEL2    = {0.196, 0.216, 0.255};
const RGB COL_ACCENT    = {0.298, 0.686, 0.875};
const RGB COL_ACCENT_D  = {0.180, 0.480, 0.650};
const RGB COL_TEXT      = {0.886, 0.910, 0.941};
const RGB COL_TEXT_DIM  = {0.560, 0.600, 0.650};
const RGB COL_BORDER    = {0.250, 0.270, 0.310};
const RGB COL_PLOTBG    = {0.075, 0.082, 0.098};

void set_color(cairo_t *cr, const RGB &c, double a = 1.0) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// --- Pango text helpers -----------------------------------------------------

void draw_text(cairo_t *cr, const std::string &s, double x, double y,
               const RGB &col, double size = 13, bool bold = false,
               PangoAlignment align = PANGO_ALIGN_LEFT, double wrapw = -1) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Sans");
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, s.c_str(), -1);
    if (wrapw > 0) {
        pango_layout_set_width(layout, (int)(wrapw * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(layout, align);
    }
    set_color(cr, col);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
}

void text_size(cairo_t *cr, const std::string &s, double size, bool bold,
               double &w, double &h) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Sans");
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, s.c_str(), -1);
    int pw, ph;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    w = pw; h = ph;
    pango_font_description_free(desc);
    g_object_unref(layout);
}

double now_seconds() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

std::string fmt_num(double v) {
    char buf[48];
    double a = std::fabs(v);
    if (v == 0) std::snprintf(buf, sizeof(buf), "0");
    else if (a >= 1e5 || a < 1e-3) std::snprintf(buf, sizeof(buf), "%.3g", v);
    else std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------

class App {
public:
    App(NcFile &nc, Units &units) : nc_(nc), units_(units) {
        cmaps_ = builtin_colormaps();
        load_coastlines(default_coastline_path(), coast_);
        load_coastlines(default_borders_path(), borders_);   // same binary format
        if (!nc_.displayable().empty()) select_var(nc_.displayable().front());
    }

    int run();

private:
    // ---- model -----------------------------------------------------------
    NcFile &nc_;
    Units &units_;
    std::vector<Palette> cmaps_;
    int cmap_idx_ = 0;

    int cur_var_ = -1;                 // index into nc_.vars()
    std::vector<size_t> fixed_;        // index per dim position of current var
    Slice slice_;
    bool flip_y_ = false;
    bool auto_range_ = false;           // start with a fixed (global) range
    bool symmetric_ = false;            // force the scale symmetric around zero
    bool reversed_ = false;             // reverse the colour scale direction
    double vmin_ = 0, vmax_ = 1;
    int editing_ = 0;                   // 0 none, 1 editing min, 2 editing max
    std::string edit_buf_;

    // Cached colormap-rendered field image, reused across redraws (e.g. resize)
    // and only rebuilt when the data or its appearance actually changes.
    cairo_surface_t *data_img_ = nullptr;
    long slice_version_ = 0;           // bumped whenever the slice is reloaded
    long img_ver_ = -1;
    int img_cmap_ = -1, img_nx_ = -1, img_ny_ = -1;
    double img_vmin_ = 0, img_vmax_ = 0;
    bool img_flip_ = false;
    bool img_rev_ = false;

    std::vector<double> ycoord_, xcoord_;
    Coastlines coast_, borders_;
    bool show_coast_ = true;           // coastline overlay on by default
    bool show_borders_ = false;        // country-border overlay off by default
    bool geographic_ = false;          // current axes are lon/lat

    // Cached vector overlay (coastlines or borders), rebuilt only when the plot
    // geometry changes — never on hover, animation or recolour.
    struct LineOverlay {
        cairo_surface_t *img = nullptr;
        double s = -1, vx0 = -1, vy0 = -1, vw = -1, vh = -1;
        int nx = -1, ny = -1, var = -1;
        bool flip = false;
    };
    LineOverlay coast_ov_, borders_ov_;
    bool playing_ = false;
    double last_frame_ = 0;
    double fps_ = 4.0;
    int anim_dim_ = -1;                // dim position used for animation

    double mouse_x_ = -1, mouse_y_ = -1;

    // ---- X / cairo -------------------------------------------------------
    Display *dpy_ = nullptr;
    Window win_ = 0;
    cairo_surface_t *surf_ = nullptr;  // on-screen (front) surface
    cairo_t *front_cr_ = nullptr;      // used only to blit the back buffer
    cairo_surface_t *back_ = nullptr;  // off-screen double buffer
    cairo_t *cr_ = nullptr;            // all drawing goes here (into back_)
    int width_ = 1180, height_ = 760;
    Atom wm_delete_ = 0;

    void make_back_buffer();           // (re)create back_/cr_ at current size

    // ---- metadata window -------------------------------------------------
    Window meta_win_ = 0;
    cairo_surface_t *meta_surf_ = nullptr;
    cairo_t *meta_cr_ = nullptr;
    int meta_w_ = 720, meta_h_ = 640;
    int meta_scroll_ = 0;                // pixels scrolled from top
    std::vector<NcFile::MetaLine> meta_lines_;

    void open_meta_window();
    void close_meta_window();
    void draw_meta();
    void scroll_meta(int dy);

    // Mouse text selection in the metadata window (monospace, so columns map to
    // a fixed character advance). Anchor/caret are document (line, column) pairs.
    double meta_char_w_ = 0;
    bool meta_selecting_ = false;
    int sel_a_line_ = 0, sel_a_col_ = 0, sel_c_line_ = 0, sel_c_col_ = 0;
    std::string meta_sel_text_;        // current selection, served to X clients
    Atom clipboard_atom_ = 0, targets_atom_ = 0, utf8_atom_ = 0;
    void meta_point_from_mouse(int mx, int my, int &line, int &col) const;
    void meta_selection_ordered(int &l0, int &c0, int &l1, int &c1) const;
    void meta_commit_selection();      // build text + own PRIMARY/CLIPBOARD

    // ---- time-series window ----------------------------------------------
    Window ts_win_ = 0;
    cairo_surface_t *ts_surf_ = nullptr;
    cairo_t *ts_cr_ = nullptr;
    int ts_w_ = 760, ts_h_ = 440;
    std::vector<double> ts_vals_;        // series values (NaN = missing)
    std::vector<double> ts_x_;           // series x coordinate (e.g. time)
    bool ts_is_time_ = false;
    std::string ts_xunits_, ts_xcal_;
    std::string ts_title_, ts_subtitle_;
    int ts_cur_idx_ = -1;                // index to highlight (current frame)
    double ts_ymin_ = 0, ts_ymax_ = 1;

    void open_ts_window(size_t yidx, size_t xidx);
    void close_ts_window();
    void draw_ts();

    // Reusable line-chart renderer (used by the time-series window and by the
    // main plot when the current variable is 1-D).
    void draw_series(cairo_t *cr, const Rect &R,
                     const std::vector<double> &xv, const std::vector<double> &yv,
                     double ymin, double ymax, bool is_time,
                     const std::string &xunits, const std::string &xcal, int cur_idx);

    // ---- 1-D variable shown as a line plot in the main window ------------
    std::vector<double> line_vals_, line_x_;
    bool line_is_time_ = false;
    std::string line_xunits_, line_xcal_;
    double line_ymin_ = 0, line_ymax_ = 1;
    void setup_line();                 // populate the above from the current 1-D var
    void draw_plot_line();             // render the line plot in r_plot_

    // Geometry of the drawn field image, stored by draw_plot for hit-testing.
    double plot_ox_ = 0, plot_oy_ = 0, plot_s_ = 0, plot_dw_ = 0, plot_dh_ = 0;
    int plot_nx_ = 0, plot_ny_ = 0;
    double plot_vx0_ = 0, plot_vy0_ = 0; // visible window top-left in image pixels

    // Zoom region as fractions [0,1] of the full field (display orientation).
    bool zoomed_ = false;
    double zoom_fx0_ = 0, zoom_fy0_ = 0, zoom_fx1_ = 1, zoom_fy1_ = 1;
    // Rubber-band selection in progress (window pixels).
    bool selecting_ = false;
    double sel_x0_ = 0, sel_y0_ = 0;
    void end_selection(int x, int y);
    // Pan the zoom window across the full field (scrollbars + mouse wheel).
    Rect r_plot_vsb_, r_plot_hsb_;     // vertical / horizontal scrollbar tracks
    int plot_sb_drag_ = 0;             // 0 none, 1 vertical, 2 horizontal
    void pan_zoom(double dx, double dy);          // shift window by fractions
    void plot_scroll_to(double mx, double my, int axis);  // drag a scrollbar

    // ---- layout rectangles ----------------------------------------------
    // Fixed chrome dimensions, shared by layout() and the default window size.
    static constexpr double PAD = 12, TOOLBAR_H = 52, SIDEBAR_W = 230,
                            COLORBAR_W = 104, HEADER_H = 30, CONTROLS_H = 40;
    void apply_default_size();         // size the window for a global lon/lat grid

    Rect r_sidebar_, r_plot_, r_colorbar_, r_toolbar_, r_header_;
    Rect r_first_, r_play_, r_prev_, r_next_, r_cmap_, r_flip_, r_range_, r_coast_, r_borders_, r_info_;
    Rect r_cbmax_, r_cbmin_;           // editable colorbar bound fields
    Rect r_sym_;                       // symmetric-around-zero toggle
    Rect r_cmaprev_;                   // reverse-colour-scale toggle
    std::vector<Rect> r_varitems_;     // parallel to nc_.displayable()
    int sidebar_scroll_ = 0;           // pixels scrolled from top
    Rect r_sb_track_;                  // sidebar scrollbar hit region (empty if none)
    bool drag_sidebar_ = false;        // dragging the scrollbar thumb
    void scroll_sidebar(int dy);
    void set_sidebar_scroll_from_y(double my);
    struct SliderUI { Rect track; int dimpos; };
    std::vector<SliderUI> sliders_;
    int drag_slider_ = -1;

    // ---- methods ---------------------------------------------------------
    void select_var(int vidx);
    void reload_slice();
    void compute_range();
    void apply_symmetric();            // force [-M, +M] with M = max(|lo|,|hi|)
    int time_dim_pos() const;          // dim position that is a time axis, or -1

    void layout();
    void render();
    void draw_toolbar();
    void draw_sidebar();
    void draw_var_tooltip();           // full name/long_name/units on hover
    void draw_button_tooltip();        // explains each toolbar / playback button
    void draw_cmap_menu();             // colour-map dropdown, when open
    void draw_wait_overlay(const std::string &msg);  // modal "please wait" popup
    bool cmap_open_ = false;           // colour-map dropdown visible
    std::vector<Rect> r_cmap_items_;   // option rows (parallel to cmaps_)
    void draw_plot();
    void draw_overlay(const Coastlines &src, LineOverlay &ov, const RGB &core,
                      double core_w, double halo_w, bool dashed,
                      double ox, double oy, double s, int nx, int ny,
                      double vx0, double vy0, double vw, double vh);
    void draw_colorbar();
    void draw_sliders();

    void on_button(int bx, int by, int button);
    void on_motion(int mx, int my);
    void on_key(KeySym ks);
    bool handle_edit_key(XKeyEvent &ev, KeySym ks); // returns false to commit/cancel
    void commit_edit();
    void step_anim(int delta);

    std::string dim_label(int dimpos, size_t idx) const;
    const NcVar &cur() const { return nc_.vars()[cur_var_]; }
};

// ---------------------------------------------------------------------------

int App::time_dim_pos() const {
    if (cur_var_ < 0) return -1;
    const NcVar &v = cur();
    for (int p = 0; p < v.ndims - 2; ++p) {
        std::string u = nc_.coord_units(v.dimids[p]);
        if (units_.is_time(u)) return p;
    }
    return -1;
}

void App::select_var(int vidx) {
    cur_var_ = vidx;
    const NcVar &v = cur();
    fixed_.assign(v.ndims, 0);
    zoomed_ = false; selecting_ = false;   // a different grid: start unzoomed
    editing_ = 0;

    // A 1-D variable is shown as a line plot rather than a 2-D field.
    if (v.ndims == 1) { anim_dim_ = -1; geographic_ = false; setup_line(); return; }

    // y/x coordinate variables (last two dims).
    ycoord_ = nc_.coord_values(v.dimids[v.ndims - 2]);
    xcoord_ = nc_.coord_values(v.dimids[v.ndims - 1]);

    // North-up by default when latitude increases with index.
    flip_y_ = (ycoord_.size() >= 2 && ycoord_.front() < ycoord_.back());

    anim_dim_ = time_dim_pos();
    if (anim_dim_ < 0 && v.ndims > 2) anim_dim_ = 0;

    // Decide whether the two plotted axes are geographic (lon/lat), which is a
    // prerequisite for the coastline overlay.
    auto is_lon = [&](int dimid) {
        std::string u = nc_.coord_units(dimid), n = nc_.dim(dimid).name;
        return u.find("east") != std::string::npos || n == "lon" ||
               n == "longitude" || n == "x";
    };
    auto is_lat = [&](int dimid) {
        std::string u = nc_.coord_units(dimid), n = nc_.dim(dimid).name;
        return u.find("north") != std::string::npos || n == "lat" ||
               n == "latitude" || n == "y";
    };
    geographic_ = !xcoord_.empty() && !ycoord_.empty() &&
                  is_lon(v.dimids[v.ndims - 1]) && is_lat(v.dimids[v.ndims - 2]);

    // Fixed range from a quick global min/max scan, so the colour scale is
    // stable across time steps. Falls back to per-slice auto if the scan fails.
    // For large variables this scan can take a while (it runs before the window
    // is even mapped at start-up), so tell the user it is under way.
    editing_ = 0;
    size_t total = 1;
    for (int p = 0; p < v.ndims; ++p) total *= nc_.dim(v.dimids[p]).len;
    if (total > 1000000) {
        std::fprintf(stderr,
                     "ncvista: computing value range for '%s' — this may take a moment…\n",
                     v.name.c_str());
        std::fflush(stderr);
        draw_wait_overlay("Computing value range…");  // no-op before the window exists
    }
    double lo, hi;
    if (nc_.var_minmax(v, lo, hi) && hi > lo) {
        auto_range_ = false;
        vmin_ = lo; vmax_ = hi;
        apply_symmetric();
    } else {
        auto_range_ = true;
    }
    reload_slice();
}

void App::reload_slice() {
    if (cur_var_ < 0) return;
    slice_ = nc_.read_slice(cur(), fixed_);
    ++slice_version_;                 // invalidates the cached field image
    if (auto_range_) compute_range();
}

// Read a 1-D variable into a series plotted as a line in the main window.
void App::setup_line() {
    const NcVar &v = cur();
    line_vals_ = nc_.read_series(v, fixed_, 0, 0, 0);   // whole variable along dim 0
    int d = v.dimids[0];
    line_x_ = nc_.coord_values(d);
    line_xunits_ = nc_.coord_units(d);
    line_xcal_ = nc_.coord_calendar(d);
    line_is_time_ = units_.is_time(line_xunits_);

    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (double y : line_vals_)
        if (std::isfinite(y)) { lo = std::min(lo, y); hi = std::max(hi, y); }
    if (lo > hi) { lo = 0; hi = 1; }
    if (hi == lo) { hi = lo + 1; }
    double margin = 0.05 * (hi - lo);
    line_ymin_ = lo - margin; line_ymax_ = hi + margin;
}

void App::compute_range() {
    if (slice_.valid) { vmin_ = slice_.dmin; vmax_ = slice_.dmax; }
    if (vmax_ <= vmin_) vmax_ = vmin_ + 1.0;
    apply_symmetric();
}

// Make the colour scale symmetric about zero: [-M, +M] with M the larger of
// |vmin| and |vmax|, so diverging fields are centred on zero.
void App::apply_symmetric() {
    if (!symmetric_) return;
    double m = std::max(std::fabs(vmin_), std::fabs(vmax_));
    if (m <= 0) m = 1.0;
    vmin_ = -m; vmax_ = m;
}

std::string App::dim_label(int dimpos, size_t idx) const {
    const NcVar &v = cur();
    int dimid = v.dimids[dimpos];
    std::string u = nc_.coord_units(dimid);
    std::vector<double> cv = nc_.coord_values(dimid);
    if (idx < cv.size()) {
        if (units_.is_time(u)) {
            std::string cal = nc_.coord_calendar(dimid);
            return units_.format_time(u, cal, cv[idx]);
        }
        return fmt_num(cv[idx]) + (u.empty() ? "" : " " + u);
    }
    return std::string("#") + std::to_string(idx);
}

// ---- layout ---------------------------------------------------------------

void App::apply_default_size() {
    // Size the window so the plot canvas holds a standard global lon/lat grid
    // (360 x 180, a 2:1 map). The per-cell size is chosen so the whole window is
    // about TARGET_W wide while the plot still fully fills the 2:1 aspect.
    const int GRID_NX = 360, GRID_NY = 180;
    const double inner = 16;            // inner padding used inside the plot rect
    const double TARGET_W = 1500;       // desired initial window width (px)
    const double chrome_w = SIDEBAR_W + COLORBAR_W + 3 * PAD;
    double cell = std::max(2.0, (TARGET_W - chrome_w - inner) / GRID_NX);
    int nslid = (cur_var_ >= 0) ? std::max(0, cur().ndims - 2) : 0;
    double slider_area = nslid > 0 ? (nslid * 34 + 16) : CONTROLS_H;
    double plot_w = GRID_NX * cell + inner;
    double plot_h = GRID_NY * cell + inner;
    width_  = (int)std::lround(plot_w + chrome_w);
    height_ = (int)std::lround(plot_h + slider_area + TOOLBAR_H + HEADER_H + 2 * PAD);
}

void App::layout() {
    const double pad = PAD;
    const double toolbar_h = TOOLBAR_H;
    const double sidebar_w = SIDEBAR_W;

    r_toolbar_ = {0, 0, (double)width_, toolbar_h};

    // Toolbar buttons (playback controls now live in the slider area below).
    // All buttons share one width: the widest label any of them can show, so
    // the row stays uniform regardless of the current state / colormap.
    double bw = 0;
    auto consider = [&](const std::string &s) {
        double w, h; text_size(cr_, s, 13, true, w, h); bw = std::max(bw, w);
    };
    for (const auto &c : cmaps_) consider("▦ " + c.name);
    consider("flip ✓");
    consider("coast ✓");
    consider("borders ✓");
    consider("ⓘ metadata");
    bw += 24;                              // horizontal padding inside the button

    // The fixed/auto range toggle lives above the colour scale (see
    // draw_colorbar), not in the toolbar.
    const double gap = 8;
    double bx = pad, by = 10, bh = toolbar_h - 20;
    r_cmap_    = {bx, by, bw, bh}; bx += bw + gap;
    r_flip_    = {bx, by, bw, bh}; bx += bw + gap;
    r_coast_   = {bx, by, bw, bh}; bx += bw + gap;
    r_borders_ = {bx, by, bw, bh}; bx += bw + gap;
    r_info_    = {bx, by, bw, bh};

    r_sidebar_ = {0, toolbar_h, sidebar_w, (double)height_ - toolbar_h};

    double cx = sidebar_w + pad;
    double cw = width_ - sidebar_w - 2 * pad;
    // A header row sits between the toolbar and the plot, holding the variable
    // title so it is fully visible instead of being clipped by the toolbar.
    r_header_ = {cx, toolbar_h, cw, HEADER_H};
    double cy = toolbar_h + HEADER_H;
    double ch = height_ - cy - pad;

    // Bottom area: dimension sliders. The playback buttons sit in the left
    // gutter of the time slider's row (no time axis -> a small button strip).
    // A 1-D variable (line plot) has no sliders, colour scale or playback, so
    // the plot uses the full content area.
    const NcVar *v = cur_var_ >= 0 ? &cur() : nullptr;
    bool line_mode = v && v->ndims == 1;
    int nslid = v ? std::max(0, v->ndims - 2) : 0;
    double slider_area = line_mode ? 0.0 : (nslid > 0 ? (nslid * 34 + 16) : CONTROLS_H);
    double colorbar_w = line_mode ? 0.0 : COLORBAR_W;  // editable bound fields

    r_colorbar_ = {cx + cw - colorbar_w, cy, colorbar_w, ch - slider_area};
    r_plot_ = {cx, cy, cw - colorbar_w - (colorbar_w > 0 ? pad : 0), ch - slider_area};

    // sliders — tracks start past the button gutter so the time row aligns.
    sliders_.clear();
    const double gutter = 162;
    double sy = cy + (ch - slider_area) + 8;
    double time_track_y = -1;
    if (v) {
        for (int p = 0; p < v->ndims - 2; ++p) {
            Rect track{cx + gutter, sy + 8, cw - gutter - 230, 8};
            sliders_.push_back({track, p});
            if (p == anim_dim_) time_track_y = track.y;
            sy += 34;
        }
    }

    // playback controls, left of the time slider (centred on its track);
    // suppressed for the 1-D line plot (no animation).
    if (line_mode) {
        r_first_ = r_prev_ = r_play_ = r_next_ = {0, 0, 0, 0};
    } else {
        double cby = (time_track_y >= 0) ? (time_track_y + 4 - 16)
                                         : (cy + ch - CONTROLS_H + 4);
        double cbh = 32, cbx = cx;
        r_first_ = {cbx, cby, 32, cbh}; cbx += 36;
        r_prev_  = {cbx, cby, 32, cbh}; cbx += 36;
        r_play_  = {cbx, cby, 42, cbh}; cbx += 46;
        r_next_  = {cbx, cby, 32, cbh};
    }
}

// ---- rendering -------------------------------------------------------------

void App::make_back_buffer() {
    if (cr_) { cairo_destroy(cr_); cr_ = nullptr; }
    if (back_) { cairo_surface_destroy(back_); back_ = nullptr; }
    // A surface "similar" to the window is server-side and fast to blit.
    back_ = cairo_surface_create_similar(surf_, CAIRO_CONTENT_COLOR,
                                         width_, height_);
    cr_ = cairo_create(back_);
}

void App::render() {
    if (!back_ || !cr_) return;
    layout();
    // Draw the whole frame off-screen.
    set_color(cr_, COL_BG);
    cairo_paint(cr_);
    draw_sidebar();
    draw_plot();
    draw_colorbar();
    draw_sliders();
    draw_toolbar();
    draw_cmap_menu();     // dropdown overlays the frame when open
    draw_var_tooltip();   // last, so it overlays the rest of the frame
    draw_button_tooltip();
    cairo_surface_flush(back_);

    // Blit the finished frame to the window in one operation: no flicker.
    cairo_set_source_surface(front_cr_, back_, 0, 0);
    cairo_set_operator(front_cr_, CAIRO_OPERATOR_SOURCE);
    cairo_paint(front_cr_);
    cairo_surface_flush(surf_);
    XFlush(dpy_);
}

static void draw_button(cairo_t *cr, const Rect &r, const std::string &label,
                        bool active, bool accent) {
    rounded_rect(cr, r.x, r.y, r.w, r.h, 6);
    if (accent) set_color(cr, active ? COL_ACCENT : COL_ACCENT_D);
    else set_color(cr, active ? COL_PANEL2 : COL_PANEL);
    cairo_fill_preserve(cr);
    set_color(cr, COL_BORDER);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    double tw, th;
    text_size(cr, label, 13, true, tw, th);
    draw_text(cr, label, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2,
              accent ? RGB{1, 1, 1} : COL_TEXT, 13, true);
}

void App::draw_toolbar() {
    rounded_rect(cr_, 0, 0, width_, r_toolbar_.h, 0);
    set_color(cr_, COL_PANEL);
    cairo_fill(cr_);
    cairo_set_source_rgba(cr_, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 1);
    cairo_set_line_width(cr_, 1);
    cairo_move_to(cr_, 0, r_toolbar_.h);
    cairo_line_to(cr_, width_, r_toolbar_.h);
    cairo_stroke(cr_);

    draw_button(cr_, r_cmap_, "▦ " + cmaps_[cmap_idx_].name, false, false);
    draw_button(cr_, r_flip_, flip_y_ ? "flip ✓" : "flip", flip_y_, false);
    draw_button(cr_, r_coast_,
                show_coast_ ? "coast ✓" : "coast",
                show_coast_ && geographic_, false);
    draw_button(cr_, r_borders_,
                show_borders_ ? "borders ✓" : "borders",
                show_borders_ && geographic_, false);
    draw_button(cr_, r_info_, "ⓘ metadata", meta_win_ != 0, false);

    // File name, ellipsized to the space left of the toolbar's right edge.
    const std::string &full = nc_.path();
    std::string title = full;
    size_t sl = title.find_last_of('/');
    if (sl != std::string::npos) title = title.substr(sl + 1);

    double x0 = r_info_.x + r_info_.w + 16;
    double avail = width_ - 12 - x0;
    if (avail < 20) avail = 20;
    double tw, th;
    text_size(cr_, title, 13, false, tw, th);
    draw_text(cr_, title, x0, (r_toolbar_.h - 16) / 2, COL_TEXT_DIM, 13, false,
              PANGO_ALIGN_LEFT, avail);

    // When the name doesn't fit, reveal the complete file name (no folder
    // path) on hover as a tooltip.
    bool truncated = tw > avail;
    double regionw = std::min(tw, avail);
    if (truncated && mouse_x_ >= x0 && mouse_x_ <= x0 + regionw &&
        mouse_y_ >= 0 && mouse_y_ <= r_toolbar_.h) {
        double fw, fh;
        text_size(cr_, title, 12, false, fw, fh);
        double bx = std::clamp(x0, 8.0, std::max(8.0, (double)width_ - fw - 8));
        double by = r_toolbar_.h + 6;
        rounded_rect(cr_, bx - 6, by - 4, fw + 12, fh + 8, 5);
        set_color(cr_, COL_PANEL, 0.97);
        cairo_fill_preserve(cr_);
        set_color(cr_, COL_BORDER);
        cairo_set_line_width(cr_, 1);
        cairo_stroke(cr_);
        draw_text(cr_, title, bx, by, COL_TEXT, 12, false);
    }
}

void App::draw_sidebar() {
    set_color(cr_, COL_PANEL);
    cairo_rectangle(cr_, r_sidebar_.x, r_sidebar_.y, r_sidebar_.w, r_sidebar_.h);
    cairo_fill(cr_);

    draw_text(cr_, "VARIABLES", r_sidebar_.x + 14, r_sidebar_.y + 12,
              COL_TEXT_DIM, 11, true);

    r_varitems_.clear();
    const auto &disp = nc_.displayable();
    const double ih = 44;
    scroll_sidebar(0);                         // re-clamp after resize / var change
    double iy = r_sidebar_.y + 34 - sidebar_scroll_;
    for (size_t i = 0; i < disp.size(); ++i) {
        const NcVar &v = nc_.vars()[disp[i]];
        Rect r{r_sidebar_.x + 8, iy, r_sidebar_.w - 16, ih - 6};
        r_varitems_.push_back(r);
        if (iy + ih > r_sidebar_.y && iy < r_sidebar_.y + r_sidebar_.h) {
            bool sel = (disp[i] == cur_var_);
            rounded_rect(cr_, r.x, r.y, r.w, r.h, 6);
            // Supporting variables are dimmed to set them apart from data fields.
            RGB base = v.aux ? RGB{0.135, 0.149, 0.176} : COL_PANEL2;
            set_color(cr_, sel ? COL_ACCENT_D : base);
            cairo_fill(cr_);
            draw_text(cr_, v.name, r.x + 10, r.y + 6,
                      sel ? RGB{1, 1, 1} : (v.aux ? COL_TEXT_DIM : COL_TEXT), 13,
                      true, PANGO_ALIGN_LEFT, r.w - 20);
            std::string sub = v.long_name.empty() ? "" : v.long_name;
            if (!v.units.empty())
                sub += (sub.empty() ? "" : "  ") + ("[" + units_.pretty(v.units) + "]");
            if (!sub.empty())
                draw_text(cr_, sub, r.x + 10, r.y + 23,
                          sel ? RGB{0.85, 0.92, 0.97} : COL_TEXT_DIM, 11, false,
                          PANGO_ALIGN_LEFT, r.w - 20);
        }
        iy += ih;
    }

    // Right-side scrollbar: shown only when the list overflows the viewport.
    const double track_y = r_sidebar_.y + 34;
    const double track_h = r_sidebar_.h - 34 - 8;
    const double content = disp.size() * ih;
    if (content > track_h && track_h > 0) {
        const double track_x = r_sidebar_.x + r_sidebar_.w - 7;
        double bar_h = std::max(28.0, track_h * track_h / content);
        double maxscroll = content - track_h;
        double bar_y = track_y + (track_h - bar_h) * (sidebar_scroll_ / maxscroll);
        rounded_rect(cr_, track_x, bar_y, 5, bar_h, 2.5);
        set_color(cr_, COL_ACCENT, drag_sidebar_ ? 0.9 : 0.6);
        cairo_fill(cr_);
        r_sb_track_ = {track_x - 3, track_y, 11, track_h};   // generous hit area
    } else {
        r_sb_track_ = {0, 0, 0, 0};
    }
}

// When the cursor rests on a variable whose name / long_name / units were
// clipped in the list, reveal the complete information as a hover tooltip.
void App::draw_var_tooltip() {
    if (!r_sidebar_.hit(mouse_x_, mouse_y_)) return;
    const auto &disp = nc_.displayable();
    int hit = -1;
    for (size_t i = 0; i < r_varitems_.size() && i < disp.size(); ++i)
        if (r_varitems_[i].hit(mouse_x_, mouse_y_)) { hit = (int)i; break; }
    if (hit < 0) return;

    const NcVar &v = nc_.vars()[disp[hit]];
    const double availw = r_varitems_[hit].w - 20;

    std::string l1 = v.name;
    std::string l2 = v.long_name;
    if (!v.units.empty())
        l2 += (l2.empty() ? "" : "  ") + ("[" + units_.pretty(v.units) + "]");

    double w1, h1, w2 = 0, h2 = 0;
    text_size(cr_, l1, 13, true, w1, h1);
    if (!l2.empty()) text_size(cr_, l2, 11, false, w2, h2);
    // Only worth a tooltip if something was actually clipped in the list.
    if (w1 <= availw && w2 <= availw) return;

    const double padx = 10, pady = 7, gap = 3;
    double boxw = std::max(w1, w2) + 2 * padx;
    double boxh = h1 + (l2.empty() ? 0 : h2 + gap) + 2 * pady;
    double bx = std::clamp(mouse_x_ + 14.0, 8.0, std::max(8.0, width_ - boxw - 8));
    double by = std::clamp(mouse_y_ + 16.0, 8.0, std::max(8.0, height_ - boxh - 8));

    rounded_rect(cr_, bx, by, boxw, boxh, 6);
    set_color(cr_, COL_PANEL, 0.98);
    cairo_fill_preserve(cr_);
    set_color(cr_, COL_BORDER);
    cairo_set_line_width(cr_, 1);
    cairo_stroke(cr_);

    draw_text(cr_, l1, bx + padx, by + pady, COL_TEXT, 13, true);
    if (!l2.empty())
        draw_text(cr_, l2, bx + padx, by + pady + h1 + gap, COL_TEXT_DIM, 11, false);
}

// Hovering a toolbar or playback button shows a short description of its action.
void App::draw_button_tooltip() {
    struct BT { const Rect *r; const char *desc; };
    const BT items[] = {
        {&r_first_, "Jump to first frame"},
        {&r_prev_,  "Previous frame"},
        {&r_play_,  "Play / pause animation"},
        {&r_next_,  "Next frame"},
        {&r_cmap_,  "Choose colour map"},
        {&r_flip_,  "Flip the image vertically"},
        {&r_range_, "Toggle auto / fixed colour range"},
        {&r_sym_,   "Symmetric colour scale around zero (±max|value|)"},
        {&r_cmaprev_, "Reverse the colour scale"},
        {&r_coast_, "Toggle coastline overlay"},
        {&r_borders_, "Toggle country-border overlay"},
        {&r_info_,  "Open the metadata window"},
    };
    const Rect *br = nullptr;
    const char *desc = nullptr;
    for (const auto &it : items)
        if (it.r->hit(mouse_x_, mouse_y_)) { br = it.r; desc = it.desc; break; }
    if (!desc) return;
    if (br == &r_cmap_ && cmap_open_) return;   // dropdown shows the options instead

    double tw, th;
    text_size(cr_, desc, 12, false, tw, th);
    const double padx = 8, pady = 5;
    double boxw = tw + 2 * padx, boxh = th + 2 * pady;
    double bx = std::clamp(br->x + (br->w - boxw) / 2, 8.0,
                           std::max(8.0, width_ - boxw - 8));
    double by = br->y + br->h + 6;            // below the button…
    if (by + boxh > height_ - 6) by = br->y - boxh - 6;   // …or above near the edge

    rounded_rect(cr_, bx, by, boxw, boxh, 5);
    set_color(cr_, COL_PANEL, 0.97);
    cairo_fill_preserve(cr_);
    set_color(cr_, COL_BORDER);
    cairo_set_line_width(cr_, 1);
    cairo_stroke(cr_);
    draw_text(cr_, desc, bx + padx, by + pady, COL_TEXT, 12, false);
}

// The colour-map button opens this dropdown: one row per palette, each with a
// gradient swatch, the current one highlighted.
void App::draw_cmap_menu() {
    r_cmap_items_.clear();
    if (!cmap_open_) return;

    const double rowh = 26, sw = 40;
    double mw = std::max(r_cmap_.w, 180.0);
    double mx = std::clamp(r_cmap_.x, 6.0, std::max(6.0, width_ - mw - 6));
    double my = r_cmap_.y + r_cmap_.h + 4;
    double mh = rowh * cmaps_.size() + 6;

    rounded_rect(cr_, mx, my, mw, mh, 6);
    set_color(cr_, COL_PANEL, 0.98);
    cairo_fill_preserve(cr_);
    set_color(cr_, COL_BORDER);
    cairo_set_line_width(cr_, 1);
    cairo_stroke(cr_);

    for (size_t i = 0; i < cmaps_.size(); ++i) {
        Rect r{mx, my + 3 + (double)i * rowh, mw, rowh};
        r_cmap_items_.push_back(r);
        bool sel = ((int)i == cmap_idx_);
        bool hov = r.hit(mouse_x_, mouse_y_);
        if (sel || hov) {
            rounded_rect(cr_, r.x + 3, r.y + 1, r.w - 6, r.h - 2, 4);
            set_color(cr_, sel ? COL_ACCENT_D : COL_PANEL2);
            cairo_fill(cr_);
        }
        // gradient swatch
        const Palette &cm = cmaps_[i];
        double sxp = r.x + 8, syp = r.y + 6, sh = rowh - 12;
        for (int xx = 0; xx < (int)sw; ++xx) {
            uint8_t cr8, cg, cb; cm.sample(xx / (sw - 1), cr8, cg, cb);
            cairo_set_source_rgb(cr_, cr8 / 255.0, cg / 255.0, cb / 255.0);
            cairo_rectangle(cr_, sxp + xx, syp, 1.0, sh);
            cairo_fill(cr_);
        }
        set_color(cr_, COL_BORDER);
        cairo_set_line_width(cr_, 1);
        cairo_rectangle(cr_, sxp, syp, sw, sh);
        cairo_stroke(cr_);

        draw_text(cr_, cm.name, sxp + sw + 10, r.y + (rowh - 16) / 2,
                  sel ? RGB{1, 1, 1} : COL_TEXT, 13, sel);
    }
}

// A modal "please wait" popup drawn over the last frame and pushed straight to
// the screen, so the user gets feedback before a long, blocking read. The next
// render() repaints the frame and thereby clears it.
void App::draw_wait_overlay(const std::string &msg) {
    if (!back_ || !cr_) return;
    cairo_set_operator(cr_, CAIRO_OPERATOR_OVER);
    set_color(cr_, COL_BG, 0.55);                 // dim the window behind it
    cairo_rectangle(cr_, 0, 0, width_, height_);
    cairo_fill(cr_);

    double tw, th;
    text_size(cr_, msg, 14, true, tw, th);
    double w = std::max(300.0, tw + 48), h = 86;
    double x = (width_ - w) / 2, y = (height_ - h) / 2;
    rounded_rect(cr_, x, y, w, h, 10);
    set_color(cr_, COL_PANEL);
    cairo_fill_preserve(cr_);
    set_color(cr_, COL_BORDER);
    cairo_set_line_width(cr_, 1);
    cairo_stroke(cr_);
    draw_text(cr_, msg, x + 24, y + 22, COL_TEXT, 14, true, PANGO_ALIGN_LEFT, w - 48);
    draw_text(cr_, "Please wait…", x + 24, y + 48, COL_TEXT_DIM, 12, false,
              PANGO_ALIGN_LEFT, w - 48);

    // Push the back buffer to the window immediately (blocking read follows).
    cairo_surface_flush(back_);
    cairo_set_source_surface(front_cr_, back_, 0, 0);
    cairo_set_operator(front_cr_, CAIRO_OPERATOR_SOURCE);
    cairo_paint(front_cr_);
    cairo_surface_flush(surf_);
    XFlush(dpy_);
}

void App::draw_plot() {
    const Rect &R = r_plot_;
    rounded_rect(cr_, R.x, R.y, R.w, R.h, 8);
    set_color(cr_, COL_PLOTBG);
    cairo_fill(cr_);

    const NcVar &v = cur();
    // header — drawn in its own row below the toolbar (see layout()).
    std::string head = v.name;
    if (!v.long_name.empty()) head = v.long_name + " (" + v.name + ")";
    std::string un = v.units.empty() ? "" : "  [" + units_.pretty(v.units) + "]";
    draw_text(cr_, head + un, r_header_.x + 4, r_header_.y + 5, COL_TEXT, 15, true,
              PANGO_ALIGN_LEFT, r_header_.w);

    // 1-D variables are drawn as a line plot rather than a 2-D field.
    if (v.ndims == 1) {
        plot_s_ = 0; plot_nx_ = plot_ny_ = 0;   // disable field click/hover targets
        draw_plot_line();
        return;
    }

    if (!slice_.valid || slice_.nx <= 0 || slice_.ny <= 0) {
        draw_text(cr_, "no data", R.x + R.w / 2 - 30, R.y + R.h / 2, COL_TEXT_DIM, 14);
        return;
    }

    const int nx = slice_.nx, ny = slice_.ny;

    // The colormap-mapped field image is independent of window size, so build
    // it only when the data or its appearance changes. Resizing then reuses the
    // cached image and merely rescales it during the blit — fast.
    bool stale = !data_img_ || img_ver_ != slice_version_ ||
                 img_cmap_ != cmap_idx_ || img_vmin_ != vmin_ ||
                 img_vmax_ != vmax_ || img_flip_ != flip_y_ ||
                 img_rev_ != reversed_ ||
                 img_nx_ != nx || img_ny_ != ny;
    if (stale) {
        if (data_img_) cairo_surface_destroy(data_img_);
        data_img_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nx, ny);
        unsigned char *px = cairo_image_surface_get_data(data_img_);
        int stride = cairo_image_surface_get_stride(data_img_);
        const Palette &cm = cmaps_[cmap_idx_];
        double span = (vmax_ > vmin_) ? (vmax_ - vmin_) : 1.0;
        for (int yy = 0; yy < ny; ++yy) {
            int srcrow = flip_y_ ? (ny - 1 - yy) : yy;
            uint32_t *row = (uint32_t *)(px + yy * stride);
            for (int xx = 0; xx < nx; ++xx) {
                double val = slice_.data[(size_t)srcrow * nx + xx];
                if (std::isnan(val)) {
                    row[xx] = 0xFFFFFFFFu; // missing / fill -> white
                    continue;
                }
                double t = (val - vmin_) / span;
                if (reversed_) t = 1.0 - t;
                uint8_t r, g, b;
                cm.sample(t, r, g, b);
                row[xx] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
        cairo_surface_mark_dirty(data_img_);
        img_ver_ = slice_version_; img_cmap_ = cmap_idx_;
        img_vmin_ = vmin_; img_vmax_ = vmax_; img_flip_ = flip_y_;
        img_rev_ = reversed_;
        img_nx_ = nx; img_ny_ = ny;
    }

    // Visible window in image-pixel space (full image unless zoomed in).
    double vx0 = zoomed_ ? zoom_fx0_ * nx : 0.0;
    double vy0 = zoomed_ ? zoom_fy0_ * ny : 0.0;
    double vw  = zoomed_ ? (zoom_fx1_ - zoom_fx0_) * nx : (double)nx;
    double vh  = zoomed_ ? (zoom_fy1_ - zoom_fy0_) * ny : (double)ny;

    // Fit preserving aspect ratio.
    double avail_w = R.w - 16, avail_h = R.h - 16;
    double s = std::min(avail_w / vw, avail_h / vh);
    double dw = vw * s, dh = vh * s;
    double ox = R.x + (R.w - dw) / 2, oy = R.y + (R.h - dh) / 2;

    // Remember the on-screen image geometry so clicks can be mapped to cells.
    plot_ox_ = ox; plot_oy_ = oy; plot_s_ = s; plot_dw_ = dw; plot_dh_ = dh;
    plot_nx_ = nx; plot_ny_ = ny; plot_vx0_ = vx0; plot_vy0_ = vy0;

    cairo_save(cr_);
    // Clip to the visible image rect so the zoomed crop shows only that region.
    cairo_rectangle(cr_, ox, oy, dw, dh);
    cairo_clip(cr_);
    cairo_translate(cr_, ox, oy);
    cairo_scale(cr_, s, s);
    cairo_translate(cr_, -vx0, -vy0);
    cairo_set_source_surface(cr_, data_img_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_NEAREST);
    cairo_paint(cr_);
    cairo_restore(cr_);

    // geographic overlays: coastlines (solid, near-black) and country borders
    // (dashed, dark red).
    if (geographic_) {
        if (show_coast_ && coast_.loaded)
            draw_overlay(coast_, coast_ov_, {0.05, 0.05, 0.07}, 1.1, 2.4, false,
                         ox, oy, s, nx, ny, vx0, vy0, vw, vh);
        if (show_borders_ && borders_.loaded)
            draw_overlay(borders_, borders_ov_, {0.45, 0.10, 0.10}, 1.0, 2.0, true,
                         ox, oy, s, nx, ny, vx0, vy0, vw, vh);
    }

    // Pan scrollbars along the image edges when zoomed; their thumbs show the
    // visible window's position within the full field and can be dragged.
    if (zoomed_) {
        double fh = zoom_fy1_ - zoom_fy0_, fw = zoom_fx1_ - zoom_fx0_;
        r_plot_vsb_ = {ox + dw - 8, oy, 8, dh};
        double ty = oy + zoom_fy0_ * dh, tH = std::max(20.0, fh * dh);
        rounded_rect(cr_, ox + dw - 7, ty + 1, 5, tH - 2, 2.5);
        set_color(cr_, COL_ACCENT, plot_sb_drag_ == 1 ? 0.9 : 0.55);
        cairo_fill(cr_);
        r_plot_hsb_ = {ox, oy + dh - 8, dw, 8};
        double tx = ox + zoom_fx0_ * dw, tW = std::max(20.0, fw * dw);
        rounded_rect(cr_, tx + 1, oy + dh - 7, tW - 2, 5, 2.5);
        set_color(cr_, COL_ACCENT, plot_sb_drag_ == 2 ? 0.9 : 0.55);
        cairo_fill(cr_);
    } else {
        r_plot_vsb_ = {0, 0, 0, 0};
        r_plot_hsb_ = {0, 0, 0, 0};
    }

    // Rubber-band selection rectangle takes precedence over the hover readout.
    if (selecting_) {
        double rx0 = std::clamp(std::min(sel_x0_, mouse_x_), ox, ox + dw);
        double ry0 = std::clamp(std::min(sel_y0_, mouse_y_), oy, oy + dh);
        double rx1 = std::clamp(std::max(sel_x0_, mouse_x_), ox, ox + dw);
        double ry1 = std::clamp(std::max(sel_y0_, mouse_y_), oy, oy + dh);
        cairo_rectangle(cr_, rx0, ry0, rx1 - rx0, ry1 - ry0);
        set_color(cr_, COL_ACCENT, 0.18);
        cairo_fill_preserve(cr_);
        set_color(cr_, COL_ACCENT, 0.9);
        cairo_set_line_width(cr_, 1.5);
        cairo_stroke(cr_);
        return;
    }

    // hover readout
    if (mouse_x_ >= ox && mouse_x_ <= ox + dw &&
        mouse_y_ >= oy && mouse_y_ <= oy + dh) {
        int cxp = (int)(vx0 + (mouse_x_ - ox) / s);
        int cyp = (int)(vy0 + (mouse_y_ - oy) / s);
        cxp = std::clamp(cxp, 0, nx - 1);
        cyp = std::clamp(cyp, 0, ny - 1);
        int srcrow = flip_y_ ? (ny - 1 - cyp) : cyp;
        double val = slice_.data[(size_t)srcrow * nx + cxp];
        const std::string &yname = nc_.dim(v.dimids[v.ndims - 2]).name; // lat axis
        const std::string &xname = nc_.dim(v.dimids[v.ndims - 1]).name; // lon axis
        std::string txt = "value: ";
        txt += std::isnan(val) ? "—" : fmt_num(val);
        if (!v.units.empty() && !std::isnan(val)) txt += " " + units_.pretty(v.units);
        if (srcrow < (int)ycoord_.size() && cxp < (int)xcoord_.size())
            txt += "    @ (" + fmt_num(xcoord_[cxp]) + ", " +
                   fmt_num(ycoord_[srcrow]) + ")";
        // Array indices of the latitude (row) and longitude (column).
        txt += "   " + yname + "[" + std::to_string(srcrow) + "] " +
               xname + "[" + std::to_string(cxp) + "]";

        double tw, th;
        text_size(cr_, txt, 12, true, tw, th);
        double bxp = std::min(mouse_x_ + 14, R.x + R.w - tw - 16);
        double byp = std::min(mouse_y_ + 14, R.y + R.h - th - 12);
        rounded_rect(cr_, bxp - 6, byp - 4, tw + 12, th + 8, 5);
        set_color(cr_, COL_PANEL, 0.92);
        cairo_fill(cr_);
        draw_text(cr_, txt, bxp, byp, COL_TEXT, 12, true);

        // crosshair
        set_color(cr_, COL_ACCENT, 0.6);
        cairo_set_line_width(cr_, 1);
        cairo_move_to(cr_, mouse_x_, oy); cairo_line_to(cr_, mouse_x_, oy + dh);
        cairo_move_to(cr_, ox, mouse_y_); cairo_line_to(cr_, ox + dw, mouse_y_);
        cairo_stroke(cr_);
    }
}

void App::draw_overlay(const Coastlines &src, LineOverlay &ov, const RGB &core,
                       double core_w, double halo_w, bool dashed,
                       double ox, double oy, double s, int nx, int ny,
                       double vx0, double vy0, double vw, double vh) {
    const int nxc = (int)xcoord_.size(), nyc = (int)ycoord_.size();
    if (nxc < 2 || nyc < 2) return;

    // The overlay bitmap covers only the visible (possibly zoomed) window, so
    // its size stays bounded by the plot area even at high zoom.
    const double dw = vw * s, dh = vh * s;
    const int iw = (int)std::ceil(dw), ih = (int)std::ceil(dh);
    if (iw <= 0 || ih <= 0) return;

    // Rebuild the cached overlay only when the geometry changes. Hover,
    // animation, recolour and range edits all reuse the same bitmap.
    bool stale = !ov.img || ov.s != s || ov.nx != nx ||
                 ov.ny != ny || ov.flip != flip_y_ || ov.var != cur_var_ ||
                 ov.vx0 != vx0 || ov.vy0 != vy0 ||
                 ov.vw != vw || ov.vh != vh;
    if (stale) {
        if (ov.img) cairo_surface_destroy(ov.img);
        ov.img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
        cairo_t *tcr = cairo_create(ov.img);

        const double xc0 = xcoord_.front(), xcn = xcoord_.back();
        const double yc0 = ycoord_.front(), ycn = ycoord_.back();
        const double xlo = std::min(xc0, xcn), xhi = std::max(xc0, xcn);

        // Geographic coordinate -> pixel within the (visible-window) overlay.
        auto lon_to_px = [&](double lon, double &px) -> bool {
            double L = std::fmod(lon - xlo, 360.0);
            if (L < 0) L += 360.0;
            L += xlo;                              // L in [xlo, xlo + 360)
            if (L > xhi) { double a = L - 360.0; if (a >= xlo) L = a; else return false; }
            double fx = (L - xc0) * (nx - 1) / (xcn - xc0);
            if (fx < -0.5 || fx > nx - 0.5) return false;
            px = (fx + 0.5 - vx0) * s;
            return true;
        };
        auto lat_to_py = [&](double lat, double &py) -> bool {
            double fy = (lat - yc0) * (ny - 1) / (ycn - yc0);
            if (fy < -0.5 || fy > ny - 0.5) return false;
            double yy = flip_y_ ? (ny - 1 - fy) : fy;
            py = (yy + 0.5 - vy0) * s;
            return true;
        };

        cairo_set_line_join(tcr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap(tcr, CAIRO_LINE_CAP_ROUND);
        const double maxjump = dw * 0.5;       // suppress antimeridian wrap-arounds

        // Build the path once, decimating points that fall on the same pixel,
        // then stroke it twice (halo + core).
        auto build_path = [&]() {
            for (const auto &line : src.lines) {
                bool pen = false;
                double lx = 0, ly = 0;
                for (const auto &pt : line) {
                    double px, py;
                    if (lon_to_px(pt.first, px) && lat_to_py(pt.second, py)) {
                        if (!pen || std::fabs(px - lx) >= maxjump) {
                            cairo_move_to(tcr, px, py);
                            pen = true; lx = px; ly = py;
                        } else if (std::fabs(px - lx) >= 0.75 ||
                                   std::fabs(py - ly) >= 0.75) {
                            cairo_line_to(tcr, px, py);
                            lx = px; ly = py;
                        }
                    } else {
                        pen = false;
                    }
                }
            }
        };

        const double dashes[2] = {4.0, 3.0};
        if (dashed) cairo_set_dash(tcr, dashes, 2, 0.0);

        build_path();
        cairo_set_source_rgba(tcr, 1, 1, 1, 0.45);   // light halo
        cairo_set_line_width(tcr, halo_w);
        cairo_stroke(tcr);
        build_path();
        cairo_set_source_rgba(tcr, core.r, core.g, core.b, 0.9);  // coloured core
        cairo_set_line_width(tcr, core_w);
        cairo_stroke(tcr);
        cairo_surface_flush(ov.img);
        cairo_destroy(tcr);

        ov.s = s; ov.nx = nx; ov.ny = ny;
        ov.flip = flip_y_; ov.var = cur_var_;
        ov.vx0 = vx0; ov.vy0 = vy0; ov.vw = vw; ov.vh = vh;
    }

    cairo_set_source_surface(cr_, ov.img, ox, oy);
    cairo_paint(cr_);
}

void App::draw_colorbar() {
    const Rect &R = r_colorbar_;
    // No colour scale for the 1-D line plot; drop the toggle hit-rects too.
    if (R.w < 1 || (cur_var_ >= 0 && cur().ndims == 1)) {
        r_range_ = r_sym_ = r_cmaprev_ = r_cbmax_ = r_cbmin_ = {0, 0, 0, 0};
        return;
    }

    // Toggles above the colour scale, each with a check mark / accent highlight
    // when active: "fixed" pins the range; "symmetric" centres it on zero;
    // "reverse" flips the colour scale direction.
    double btn_h = 24, btn_gap = 6;
    r_range_ = {R.x + 6, R.y + 6, R.w - 12, btn_h};
    draw_button(cr_, r_range_, auto_range_ ? "fixed" : "fixed ✓", !auto_range_, false);
    r_sym_ = {R.x + 6, R.y + 6 + (btn_h + btn_gap), R.w - 12, btn_h};
    draw_button(cr_, r_sym_, symmetric_ ? "symmetric ✓" : "symmetric", symmetric_, false);
    r_cmaprev_ = {R.x + 6, R.y + 6 + 2 * (btn_h + btn_gap), R.w - 12, btn_h};
    draw_button(cr_, r_cmaprev_, reversed_ ? "reverse ✓" : "reverse", reversed_, false);

    const double reserve = 3 * btn_h + 2 * btn_gap + 14;  // space taken by the toggles
    double barx = R.x + 8, bary = R.y + 24 + reserve;
    double barw = 22, barh = R.h - 48 - reserve;
    const Palette &cm = cmaps_[cmap_idx_];

    for (int i = 0; i < (int)barh; ++i) {
        double t = 1.0 - (double)i / barh; // top = max
        if (reversed_) t = 1.0 - t;
        uint8_t r, g, b; cm.sample(t, r, g, b);
        cairo_set_source_rgb(cr_, r / 255.0, g / 255.0, b / 255.0);
        cairo_rectangle(cr_, barx, bary + i, barw, 1.5);
        cairo_fill(cr_);
    }
    set_color(cr_, COL_BORDER);
    cairo_set_line_width(cr_, 1);
    cairo_rectangle(cr_, barx, bary, barw, barh);
    cairo_stroke(cr_);

    // Editable max (top) and min (bottom) fields, plus interior ticks.
    double fx = barx + barw + 7;
    double fw = std::max(46.0, R.x + R.w - fx - 2);
    r_cbmax_ = {fx, bary - 11, fw, 22};
    r_cbmin_ = {fx, bary + barh - 11, fw, 22};

    const int nticks = 6;
    for (int i = 1; i < nticks - 1; ++i) {
        double f = (double)i / (nticks - 1);
        double val = vmax_ - f * (vmax_ - vmin_);
        double ty = bary + f * barh;
        set_color(cr_, COL_TEXT_DIM);
        cairo_move_to(cr_, barx + barw, ty);
        cairo_line_to(cr_, barx + barw + 4, ty);
        cairo_stroke(cr_);
        draw_text(cr_, fmt_num(val), fx, ty - 8, COL_TEXT_DIM, 13);
    }

    auto draw_field = [&](const Rect &r, bool editing, const std::string &val) {
        rounded_rect(cr_, r.x, r.y, r.w, r.h, 4);
        set_color(cr_, editing ? COL_PANEL2 : COL_PANEL);
        cairo_fill_preserve(cr_);
        set_color(cr_, editing ? COL_ACCENT : COL_BORDER);
        cairo_set_line_width(cr_, editing ? 1.5 : 1);
        cairo_stroke(cr_);
        std::string s = editing ? (val + "|") : val;
        draw_text(cr_, s, r.x + 5, r.y + 3, editing ? RGB{1, 1, 1} : COL_TEXT, 13,
                  false, PANGO_ALIGN_LEFT, r.w - 8);
    };
    draw_field(r_cbmax_, editing_ == 2, editing_ == 2 ? edit_buf_ : fmt_num(vmax_));
    draw_field(r_cbmin_, editing_ == 1, editing_ == 1 ? edit_buf_ : fmt_num(vmin_));
}

void App::draw_sliders() {
    if (cur_var_ < 0) return;
    if (cur().ndims == 1) return;      // 1-D line plot: no sliders / playback
    const NcVar &v = cur();
    for (const auto &sl : sliders_) {
        int p = sl.dimpos;
        int dimid = v.dimids[p];
        size_t len = nc_.dim(dimid).len;
        std::string nm = nc_.dim(dimid).name;
        bool is_t = (p == anim_dim_);

        // label — omitted on the time row, where the playback buttons sit
        if (!is_t) {
            draw_text(cr_, nm, r_sidebar_.w + 12 + 12, sl.track.y - 6,
                      COL_TEXT, 12, true);
        }

        // track
        rounded_rect(cr_, sl.track.x, sl.track.y, sl.track.w, sl.track.h, 4);
        set_color(cr_, COL_PANEL2);
        cairo_fill(cr_);

        double frac = (len > 1) ? (double)fixed_[p] / (len - 1) : 0.0;
        double knobx = sl.track.x + frac * sl.track.w;
        // filled portion
        rounded_rect(cr_, sl.track.x, sl.track.y, frac * sl.track.w, sl.track.h, 4);
        set_color(cr_, COL_ACCENT);
        cairo_fill(cr_);
        // knob
        set_color(cr_, COL_ACCENT);
        cairo_arc(cr_, knobx, sl.track.y + sl.track.h / 2, 8, 0, 2 * M_PI);
        cairo_fill(cr_);
        set_color(cr_, COL_BG);
        cairo_arc(cr_, knobx, sl.track.y + sl.track.h / 2, 4, 0, 2 * M_PI);
        cairo_fill(cr_);

        // current value text
        std::string vtxt = dim_label(p, fixed_[p]) + "   (" +
                           std::to_string(fixed_[p] + 1) + "/" +
                           std::to_string(len) + ")";
        draw_text(cr_, vtxt, sl.track.x + sl.track.w + 14, sl.track.y - 6,
                  COL_TEXT, 12, false);
    }

    // playback controls, moved from the toolbar into the slider area
    draw_button(cr_, r_first_, "⏮", false, false);
    draw_button(cr_, r_prev_, "◀", false, false);
    draw_button(cr_, r_play_, playing_ ? "❚❚" : "▶", playing_, true);
    draw_button(cr_, r_next_, "▶", false, false);
}

// ---- interaction -----------------------------------------------------------

void App::step_anim(int delta) {
    if (anim_dim_ < 0) return;
    size_t len = nc_.dim(cur().dimids[anim_dim_]).len;
    if (len == 0) return;
    long n = (long)fixed_[anim_dim_] + delta;
    n = ((n % (long)len) + (long)len) % (long)len;
    fixed_[anim_dim_] = (size_t)n;
    reload_slice();
}

void App::on_button(int bx, int by, int button) {
    // Right button resets any zoom.
    if (button == Button3) {
        if (zoomed_) zoomed_ = false;
        return;
    }
    if (button == Button1) {
        // Colour-map dropdown is modal: a click either picks a row or closes it.
        if (cmap_open_) {
            for (size_t i = 0; i < r_cmap_items_.size(); ++i)
                if (r_cmap_items_[i].hit(bx, by)) { cmap_idx_ = (int)i; cmap_open_ = false; return; }
            cmap_open_ = false;
            return;
        }
        // Colorbar bound fields: click to edit min / max (type the new value;
        // Enter applies, Esc cancels, empty entry leaves the bound unchanged).
        if (r_cbmax_.hit(bx, by)) { editing_ = 2; edit_buf_.clear(); return; }
        if (r_cbmin_.hit(bx, by)) { editing_ = 1; edit_buf_.clear(); return; }
        if (editing_) commit_edit();  // clicking elsewhere commits the edit

        if (r_first_.hit(bx, by)) {
            playing_ = false;
            if (anim_dim_ >= 0 && fixed_[anim_dim_] != 0) { fixed_[anim_dim_] = 0; reload_slice(); }
            return;
        }
        if (r_play_.hit(bx, by)) { playing_ = !playing_; last_frame_ = now_seconds(); return; }
        if (r_prev_.hit(bx, by)) { playing_ = false; step_anim(-1); return; }
        if (r_next_.hit(bx, by)) { playing_ = false; step_anim(+1); return; }
        if (r_cmap_.hit(bx, by)) { cmap_open_ = true; return; }
        if (r_flip_.hit(bx, by)) { flip_y_ = !flip_y_; return; }
        if (r_range_.hit(bx, by)) {
            auto_range_ = !auto_range_;
            if (auto_range_) {
                compute_range();                  // per current slice
            } else {
                double lo, hi;                    // back to fixed global range
                if (nc_.var_minmax(cur(), lo, hi) && hi > lo) { vmin_ = lo; vmax_ = hi; }
                apply_symmetric();
            }
            return;
        }
        if (r_sym_.hit(bx, by)) {
            symmetric_ = !symmetric_;
            if (auto_range_) {
                compute_range();                  // recompute, then (un)symmetrise
            } else {
                double lo, hi;
                if (nc_.var_minmax(cur(), lo, hi) && hi > lo) { vmin_ = lo; vmax_ = hi; }
                apply_symmetric();
            }
            return;
        }
        if (r_cmaprev_.hit(bx, by)) { reversed_ = !reversed_; return; }
        if (r_coast_.hit(bx, by)) { show_coast_ = !show_coast_; return; }
        if (r_borders_.hit(bx, by)) { show_borders_ = !show_borders_; return; }
        if (r_info_.hit(bx, by)) {
            if (meta_win_) close_meta_window();
            else open_meta_window();
            return;
        }
        // sidebar scrollbar: start dragging the thumb (or jump to click)
        if (r_sb_track_.hit(bx, by)) {
            drag_sidebar_ = true;
            set_sidebar_scroll_from_y(by);
            return;
        }
        // variable list
        const auto &disp = nc_.displayable();
        for (size_t i = 0; i < r_varitems_.size() && i < disp.size(); ++i) {
            if (r_varitems_[i].hit(bx, by)) { select_var(disp[i]); return; }
        }
        // sliders
        for (size_t i = 0; i < sliders_.size(); ++i) {
            Rect hit = sliders_[i].track;
            hit.y -= 10; hit.h += 20; hit.x -= 8; hit.w += 16;
            if (hit.hit(bx, by)) {
                drag_slider_ = (int)i;
                on_motion(bx, by); // jump knob to click
                return;
            }
        }
        // Pan scrollbars (only present when zoomed): drag to scroll the view.
        if (zoomed_ && r_plot_vsb_.hit(bx, by)) {
            plot_sb_drag_ = 1; plot_scroll_to(bx, by, 1); return;
        }
        if (zoomed_ && r_plot_hsb_.hit(bx, by)) {
            plot_sb_drag_ = 2; plot_scroll_to(bx, by, 2); return;
        }
        // Press inside the field image starts a rubber-band: a small drag is
        // treated as a click (time series), a larger one zooms (see release).
        if (plot_s_ > 0 && plot_nx_ > 0 && plot_ny_ > 0 &&
            bx >= plot_ox_ && bx <= plot_ox_ + plot_dw_ &&
            by >= plot_oy_ && by <= plot_oy_ + plot_dh_) {
            selecting_ = true;
            sel_x0_ = bx; sel_y0_ = by;
            return;
        }
    }
}

// Finish a rubber-band drag in the plot: a tiny rectangle opens the time
// series at that cell, a larger one zooms into the selected region.
void App::end_selection(int x, int y) {
    selecting_ = false;
    if (plot_s_ <= 0 || plot_nx_ <= 0 || plot_ny_ <= 0) return;

    double x0 = std::clamp(std::min(sel_x0_, (double)x), plot_ox_, plot_ox_ + plot_dw_);
    double x1 = std::clamp(std::max(sel_x0_, (double)x), plot_ox_, plot_ox_ + plot_dw_);
    double y0 = std::clamp(std::min(sel_y0_, (double)y), plot_oy_, plot_oy_ + plot_dh_);
    double y1 = std::clamp(std::max(sel_y0_, (double)y), plot_oy_, plot_oy_ + plot_dh_);

    if (x1 - x0 < 5 || y1 - y0 < 5) {       // treat as a click
        int cxp = std::clamp((int)(plot_vx0_ + (sel_x0_ - plot_ox_) / plot_s_),
                             0, plot_nx_ - 1);
        int cyp = std::clamp((int)(plot_vy0_ + (sel_y0_ - plot_oy_) / plot_s_),
                             0, plot_ny_ - 1);
        int srcrow = flip_y_ ? (plot_ny_ - 1 - cyp) : cyp;
        open_ts_window((size_t)srcrow, (size_t)cxp);
        return;
    }

    // Screen rect -> absolute image pixels -> fractions of the full field.
    double ix0 = plot_vx0_ + (x0 - plot_ox_) / plot_s_;
    double ix1 = plot_vx0_ + (x1 - plot_ox_) / plot_s_;
    double iy0 = plot_vy0_ + (y0 - plot_oy_) / plot_s_;
    double iy1 = plot_vy0_ + (y1 - plot_oy_) / plot_s_;
    zoom_fx0_ = std::clamp(ix0 / plot_nx_, 0.0, 1.0);
    zoom_fx1_ = std::clamp(ix1 / plot_nx_, 0.0, 1.0);
    zoom_fy0_ = std::clamp(iy0 / plot_ny_, 0.0, 1.0);
    zoom_fy1_ = std::clamp(iy1 / plot_ny_, 0.0, 1.0);
    zoomed_ = true;
}

// Shift the zoom window by (dx, dy) fractions, keeping its size and staying
// within the full field.
void App::pan_zoom(double dx, double dy) {
    if (!zoomed_) return;
    double w = zoom_fx1_ - zoom_fx0_, h = zoom_fy1_ - zoom_fy0_;
    double nx0 = std::clamp(zoom_fx0_ + dx, 0.0, 1.0 - w);
    double ny0 = std::clamp(zoom_fy0_ + dy, 0.0, 1.0 - h);
    zoom_fx0_ = nx0; zoom_fx1_ = nx0 + w;
    zoom_fy0_ = ny0; zoom_fy1_ = ny0 + h;
}

// Position the zoom window from a scrollbar drag so its thumb is centred on the
// pointer (axis 1 = vertical, 2 = horizontal).
void App::plot_scroll_to(double mx, double my, int axis) {
    if (!zoomed_) return;
    if (axis == 1 && plot_dh_ > 0) {
        double h = zoom_fy1_ - zoom_fy0_;
        double f = std::clamp((my - plot_oy_) / plot_dh_ - h / 2, 0.0, 1.0 - h);
        zoom_fy0_ = f; zoom_fy1_ = f + h;
    } else if (axis == 2 && plot_dw_ > 0) {
        double w = zoom_fx1_ - zoom_fx0_;
        double f = std::clamp((mx - plot_ox_) / plot_dw_ - w / 2, 0.0, 1.0 - w);
        zoom_fx0_ = f; zoom_fx1_ = f + w;
    }
}

void App::on_motion(int mx, int my) {
    mouse_x_ = mx; mouse_y_ = my;
    if (plot_sb_drag_) { plot_scroll_to(mx, my, plot_sb_drag_); return; }
    if (drag_sidebar_) { set_sidebar_scroll_from_y(my); return; }
    if (drag_slider_ >= 0 && drag_slider_ < (int)sliders_.size()) {
        const SliderUI &sl = sliders_[drag_slider_];
        int p = sl.dimpos;
        size_t len = nc_.dim(cur().dimids[p]).len;
        double frac = (mx - sl.track.x) / sl.track.w;
        frac = std::clamp(frac, 0.0, 1.0);
        size_t idx = (size_t)std::lround(frac * (len - 1));
        if (idx != fixed_[p]) { fixed_[p] = idx; reload_slice(); }
    }
}

void App::on_key(KeySym ks) {
    switch (ks) {
        case XK_space: playing_ = !playing_; last_frame_ = now_seconds(); break;
        case XK_Right: playing_ = false; step_anim(+1); break;
        case XK_Left:  playing_ = false; step_anim(-1); break;
        case XK_Home:
            playing_ = false;
            if (anim_dim_ >= 0 && fixed_[anim_dim_] != 0) { fixed_[anim_dim_] = 0; reload_slice(); }
            break;
        case XK_Up: {
            const auto &d = nc_.displayable();
            auto it = std::find(d.begin(), d.end(), cur_var_);
            if (it != d.begin() && it != d.end()) select_var(*(it - 1));
            break;
        }
        case XK_Down: {
            const auto &d = nc_.displayable();
            auto it = std::find(d.begin(), d.end(), cur_var_);
            if (it != d.end() && (it + 1) != d.end()) select_var(*(it + 1));
            break;
        }
        case XK_c: cmap_idx_ = (cmap_idx_ + 1) % cmaps_.size(); break;
        case XK_f: flip_y_ = !flip_y_; break;
        case XK_l: show_coast_ = !show_coast_; break;
        case XK_b: show_borders_ = !show_borders_; break;
        case XK_a:
            auto_range_ = !auto_range_;
            if (auto_range_) {
                compute_range();
            } else {
                double lo, hi;
                if (nc_.var_minmax(cur(), lo, hi) && hi > lo) { vmin_ = lo; vmax_ = hi; }
                apply_symmetric();
            }
            break;
        case XK_s:
            symmetric_ = !symmetric_;
            if (auto_range_) {
                compute_range();
            } else {
                double lo, hi;
                if (nc_.var_minmax(cur(), lo, hi) && hi > lo) { vmin_ = lo; vmax_ = hi; }
                apply_symmetric();
            }
            break;
        case XK_r: reversed_ = !reversed_; break;
        case XK_plus: case XK_equal: fps_ = std::min(30.0, fps_ + 1); break;
        case XK_minus: fps_ = std::max(1.0, fps_ - 1); break;
        case XK_m: case XK_i:
            if (meta_win_) close_meta_window(); else open_meta_window();
            break;
        default: break;
    }
}

void App::commit_edit() {
    if (!editing_) return;
    char *end = nullptr;
    double val = std::strtod(edit_buf_.c_str(), &end);
    if (end != edit_buf_.c_str() && std::isfinite(val)) {
        if (editing_ == 1) vmin_ = val;
        else vmax_ = val;
        auto_range_ = false;            // manual bounds are fixed bounds
    }
    editing_ = 0;
    edit_buf_.clear();
}

// Returns true while still editing; false once the edit is committed/cancelled.
bool App::handle_edit_key(XKeyEvent &ev, KeySym ks) {
    if (ks == XK_Escape) { editing_ = 0; edit_buf_.clear(); return false; }
    if (ks == XK_Return || ks == XK_KP_Enter) { commit_edit(); return false; }
    if (ks == XK_Tab) {                 // commit, then jump to the other bound
        commit_edit();
        return false;
    }
    if (ks == XK_BackSpace) {
        if (!edit_buf_.empty()) edit_buf_.pop_back();
        return true;
    }
    char buf[8] = {0};
    int n = XLookupString(&ev, buf, sizeof(buf) - 1, nullptr, nullptr);
    for (int i = 0; i < n; ++i) {
        char c = buf[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
            c == 'e' || c == 'E')
            edit_buf_.push_back(c);
    }
    return true;
}

// ---- metadata window -------------------------------------------------------

void App::open_meta_window() {
    if (meta_win_) { XRaiseWindow(dpy_, meta_win_); return; }
    meta_lines_ = nc_.metadata_lines();
    meta_scroll_ = 0;

    int screen = DefaultScreen(dpy_);
    XSetWindowAttributes attrs;
    attrs.background_pixel = BlackPixel(dpy_, screen);
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                       ButtonReleaseMask | Button1MotionMask | StructureNotifyMask;
    meta_win_ = XCreateWindow(dpy_, RootWindow(dpy_, screen), 0, 0,
                              meta_w_, meta_h_, 0, DefaultDepth(dpy_, screen),
                              InputOutput, DefaultVisual(dpy_, screen),
                              CWBackPixel | CWEventMask, &attrs);

    // Selection atoms and a reset of any prior selection state.
    clipboard_atom_ = XInternAtom(dpy_, "CLIPBOARD", False);
    targets_atom_   = XInternAtom(dpy_, "TARGETS", False);
    utf8_atom_      = XInternAtom(dpy_, "UTF8_STRING", False);
    meta_selecting_ = false;
    sel_a_line_ = sel_a_col_ = sel_c_line_ = sel_c_col_ = 0;
    meta_sel_text_.clear();

    std::string base = nc_.path();
    size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    std::string title = "Metadata — " + base;
    XStoreName(dpy_, meta_win_, ("Metadata - " + base).c_str());
    Atom net_name = XInternAtom(dpy_, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy_, "UTF8_STRING", False);
    XChangeProperty(dpy_, meta_win_, net_name, utf8, 8, PropModeReplace,
                    (const unsigned char *)title.c_str(), (int)title.size());
    XSetWMProtocols(dpy_, meta_win_, &wm_delete_, 1);
    XMapWindow(dpy_, meta_win_);

    meta_surf_ = cairo_xlib_surface_create(dpy_, meta_win_,
                                           DefaultVisual(dpy_, screen),
                                           meta_w_, meta_h_);
    meta_cr_ = cairo_create(meta_surf_);

    // Measure the monospace character advance once, for column hit-testing.
    PangoLayout *layout = pango_cairo_create_layout(meta_cr_);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Monospace");
    pango_font_description_set_absolute_size(desc, 13 * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, "0000000000", -1);
    int cw = 0, chh = 0;
    pango_layout_get_pixel_size(layout, &cw, &chh);
    meta_char_w_ = cw / 10.0;
    pango_font_description_free(desc);
    g_object_unref(layout);
}

void App::close_meta_window() {
    if (!meta_win_) return;
    if (meta_cr_) { cairo_destroy(meta_cr_); meta_cr_ = nullptr; }
    if (meta_surf_) { cairo_surface_destroy(meta_surf_); meta_surf_ = nullptr; }
    XDestroyWindow(dpy_, meta_win_);
    meta_win_ = 0;
}

void App::scroll_sidebar(int dy) {
    const double ih = 44;
    int content  = (int)(nc_.displayable().size() * ih);
    int viewport = (int)(r_sidebar_.h - 34 - 8);
    int maxscroll = std::max(0, content - viewport);
    sidebar_scroll_ = std::clamp(sidebar_scroll_ + dy, 0, maxscroll);
}

// Map an absolute pointer y to a scroll offset so the thumb centre tracks it.
void App::set_sidebar_scroll_from_y(double my) {
    const double ih = 44;
    double content  = nc_.displayable().size() * ih;
    double track_y  = r_sidebar_.y + 34;
    double track_h  = r_sidebar_.h - 34 - 8;
    if (content <= track_h || track_h <= 0) return;
    double bar_h = std::max(28.0, track_h * track_h / content);
    double frac  = (my - track_y - bar_h / 2) / (track_h - bar_h);
    frac = std::clamp(frac, 0.0, 1.0);
    sidebar_scroll_ = (int)std::lround(frac * (content - track_h));
}

void App::scroll_meta(int dy) {
    const double line_h = 19;
    int content = (int)(meta_lines_.size() * line_h) + 40;
    int maxscroll = std::max(0, content - meta_h_);
    meta_scroll_ = std::clamp(meta_scroll_ + dy, 0, maxscroll);
}

// Map a window pixel to a (line, column) in the metadata document. Columns are
// byte offsets; metadata is essentially ASCII so this matches characters.
void App::meta_point_from_mouse(int mx, int my, int &line, int &col) const {
    const double line_h = 19, x0 = 18;
    int n = (int)meta_lines_.size();
    line = (int)std::floor((my - 16 + meta_scroll_) / line_h);
    line = std::clamp(line, 0, std::max(0, n - 1));
    int len = (n > 0) ? (int)meta_lines_[line].text.size() : 0;
    col = (meta_char_w_ > 0) ? (int)std::lround((mx - x0) / meta_char_w_) : 0;
    col = std::clamp(col, 0, len);
}

// Selection endpoints in reading order (l0,c0) <= (l1,c1).
void App::meta_selection_ordered(int &l0, int &c0, int &l1, int &c1) const {
    if (sel_a_line_ < sel_c_line_ ||
        (sel_a_line_ == sel_c_line_ && sel_a_col_ <= sel_c_col_)) {
        l0 = sel_a_line_; c0 = sel_a_col_; l1 = sel_c_line_; c1 = sel_c_col_;
    } else {
        l0 = sel_c_line_; c0 = sel_c_col_; l1 = sel_a_line_; c1 = sel_a_col_;
    }
}

// Assemble the selected text and take ownership of PRIMARY and CLIPBOARD so the
// selection can be pasted (middle-click) or copied (Ctrl+V) into other apps.
void App::meta_commit_selection() {
    int l0, c0, l1, c1;
    meta_selection_ordered(l0, c0, l1, c1);
    std::string out;
    for (int i = l0; i <= l1 && i < (int)meta_lines_.size(); ++i) {
        const std::string &t = meta_lines_[i].text;
        int len = (int)t.size();
        int a = (i == l0) ? std::clamp(c0, 0, len) : 0;
        int b = (i == l1) ? std::clamp(c1, 0, len) : len;
        out += t.substr(a, b - a);
        if (i < l1) out += '\n';
    }
    meta_sel_text_ = out;
    if (!meta_sel_text_.empty()) {
        XSetSelectionOwner(dpy_, XA_PRIMARY, meta_win_, CurrentTime);
        XSetSelectionOwner(dpy_, clipboard_atom_, meta_win_, CurrentTime);
    }
}

void App::draw_meta() {
    if (!meta_cr_) return;
    cairo_t *cr = meta_cr_;
    set_color(cr, COL_BG);
    cairo_paint(cr);

    const double mono = 13;
    const double line_h = 19;
    const double x0 = 18;
    double y = 16 - meta_scroll_;

    // Ordered selection range (l0,c0)..(l1,c1); l1 < l0 means no selection.
    int sl0 = 1, sc0 = 0, sl1 = 0, sc1 = 0;
    if (meta_sel_text_.size() || meta_selecting_)
        meta_selection_ordered(sl0, sc0, sl1, sc1);

    for (size_t li = 0; li < meta_lines_.size(); ++li) {
        const auto &ml = meta_lines_[li];
        if (y > -line_h && y < meta_h_) {
            RGB col = COL_TEXT;
            bool bold = false;
            switch (ml.kind) {
                case 1: col = COL_ACCENT; bold = true; break;   // section
                case 2: col = {0.66, 0.85, 0.55}; bold = true; break; // variable
                case 3: col = COL_TEXT_DIM; break;              // attribute
                case 4: col = {0.85, 0.75, 0.55}; break;        // dimension
                default: col = COL_TEXT; break;
            }
            if (ml.kind == 1) {
                // header underline bar
                set_color(cr, COL_PANEL2);
                cairo_rectangle(cr, 0, y - 3, meta_w_, line_h + 2);
                cairo_fill(cr);
            }
            // selection highlight on this line, if any
            if ((int)li >= sl0 && (int)li <= sl1) {
                int len = (int)ml.text.size();
                int a = ((int)li == sl0) ? sc0 : 0;
                int b = ((int)li == sl1) ? sc1 : len;
                if (b > a) {
                    set_color(cr, COL_ACCENT, 0.35);
                    cairo_rectangle(cr, x0 + a * meta_char_w_, y - 2,
                                    (b - a) * meta_char_w_, line_h);
                    cairo_fill(cr);
                }
            }
            // Use a monospace family for aligned metadata.
            PangoLayout *layout = pango_cairo_create_layout(cr);
            PangoFontDescription *desc = pango_font_description_new();
            pango_font_description_set_family(desc, "Monospace");
            pango_font_description_set_absolute_size(desc, mono * PANGO_SCALE);
            if (bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
            pango_layout_set_font_description(layout, desc);
            pango_layout_set_text(layout, ml.text.c_str(), -1);
            set_color(cr, col);
            cairo_move_to(cr, x0, y);
            pango_cairo_show_layout(cr, layout);
            pango_font_description_free(desc);
            g_object_unref(layout);
        }
        y += line_h;
    }

    // scrollbar hint
    int content = (int)(meta_lines_.size() * line_h) + 40;
    if (content > meta_h_) {
        double frac_h = (double)meta_h_ / content;
        double frac_y = (double)meta_scroll_ / content;
        double bar_h = std::max(24.0, meta_h_ * frac_h);
        double bar_y = meta_h_ * frac_y;
        rounded_rect(cr, meta_w_ - 8, bar_y, 5, bar_h, 2.5);
        set_color(cr, COL_ACCENT, 0.6);
        cairo_fill(cr);
    }

    cairo_surface_flush(meta_surf_);
    XFlush(dpy_);
}

// ---- time-series window ----------------------------------------------------

void App::open_ts_window(size_t yidx, size_t xidx) {
    const NcVar &v = cur();
    int sp = time_dim_pos();
    if (sp < 0) sp = anim_dim_;        // fall back to the animated dimension
    if (sp < 0) return;                // nothing to form a series over (2-D var)

    // Extracting a single column across the series dimension can be slow for
    // large (high-resolution / vertically resolved) variables, because it must
    // touch many on-disk chunks. Warn the user before that blocking read.
    size_t total = 1;
    for (int p = 0; p < v.ndims; ++p) total *= nc_.dim(v.dimids[p]).len;
    if (total > 1000000) draw_wait_overlay("Extracting time series…");

    ts_vals_ = nc_.read_series(v, fixed_, sp, yidx, xidx);
    if (ts_vals_.empty()) return;

    int sdimid = v.dimids[sp];
    ts_x_ = nc_.coord_values(sdimid);
    ts_xunits_ = nc_.coord_units(sdimid);
    ts_xcal_ = nc_.coord_calendar(sdimid);
    ts_is_time_ = units_.is_time(ts_xunits_);
    ts_cur_idx_ = (int)fixed_[sp];

    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (double y : ts_vals_)
        if (std::isfinite(y)) { lo = std::min(lo, y); hi = std::max(hi, y); }
    if (lo > hi) { lo = 0; hi = 1; }
    if (hi == lo) { hi = lo + 1; }
    double margin = 0.05 * (hi - lo);
    ts_ymin_ = lo - margin; ts_ymax_ = hi + margin;

    // Title and location subtitle. The unit is shown in the title rather than
    // on the y-axis.
    ts_title_ = v.long_name.empty() ? v.name : (v.long_name + " (" + v.name + ")");
    if (!v.units.empty()) ts_title_ += "  [" + units_.pretty(v.units) + "]";

    const std::string &yname = nc_.dim(v.dimids[v.ndims - 2]).name;
    const std::string &xname = nc_.dim(v.dimids[v.ndims - 1]).name;
    std::string loc;
    if (yidx < ycoord_.size() && xidx < xcoord_.size())
        loc = yname + " " + fmt_num(ycoord_[yidx]) + ", " + xname + " " +
              fmt_num(xcoord_[xidx]);
    loc += "  " + yname + "[" + std::to_string(yidx) + "] " + xname + "[" +
           std::to_string(xidx) + "]";
    for (int p = 0; p < v.ndims; ++p) {
        if (p == sp || p == v.ndims - 1 || p == v.ndims - 2) continue;
        loc += "    " + nc_.dim(v.dimids[p]).name + " = " + dim_label(p, fixed_[p]);
    }
    ts_subtitle_ = loc;

    if (!ts_win_) {
        int screen = DefaultScreen(dpy_);
        XSetWindowAttributes attrs;
        attrs.background_pixel = BlackPixel(dpy_, screen);
        attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                           StructureNotifyMask;
        ts_win_ = XCreateWindow(dpy_, RootWindow(dpy_, screen), 0, 0, ts_w_, ts_h_,
                                0, DefaultDepth(dpy_, screen), InputOutput,
                                DefaultVisual(dpy_, screen),
                                CWBackPixel | CWEventMask, &attrs);
        std::string title = "Time series — " + v.name;
        XStoreName(dpy_, ts_win_, ("Time series - " + v.name).c_str());
        Atom net_name = XInternAtom(dpy_, "_NET_WM_NAME", False);
        Atom utf8 = XInternAtom(dpy_, "UTF8_STRING", False);
        XChangeProperty(dpy_, ts_win_, net_name, utf8, 8, PropModeReplace,
                        (const unsigned char *)title.c_str(), (int)title.size());
        XSetWMProtocols(dpy_, ts_win_, &wm_delete_, 1);
        XMapWindow(dpy_, ts_win_);
        ts_surf_ = cairo_xlib_surface_create(dpy_, ts_win_,
                                             DefaultVisual(dpy_, screen), ts_w_, ts_h_);
        ts_cr_ = cairo_create(ts_surf_);
    } else {
        XRaiseWindow(dpy_, ts_win_);
    }
    draw_ts();
}

void App::close_ts_window() {
    if (!ts_win_) return;
    if (ts_cr_) { cairo_destroy(ts_cr_); ts_cr_ = nullptr; }
    if (ts_surf_) { cairo_surface_destroy(ts_surf_); ts_surf_ = nullptr; }
    XDestroyWindow(dpy_, ts_win_);
    ts_win_ = 0;
}

void App::draw_ts() {
    if (!ts_cr_) return;
    cairo_t *cr = ts_cr_;
    set_color(cr, COL_BG);
    cairo_paint(cr);

    draw_text(cr, ts_title_, 16, 12, COL_TEXT, 15, true, PANGO_ALIGN_LEFT, ts_w_ - 32);
    draw_text(cr, ts_subtitle_, 16, 34, COL_TEXT_DIM, 12, false, PANGO_ALIGN_LEFT,
              ts_w_ - 32);

    Rect R{0, 46, (double)ts_w_, (double)ts_h_ - 46};
    draw_series(cr, R, ts_x_, ts_vals_, ts_ymin_, ts_ymax_, ts_is_time_,
                ts_xunits_, ts_xcal_, ts_cur_idx_);

    cairo_surface_flush(ts_surf_);
    XFlush(dpy_);
}

// Draw a line chart of (xv, yv) within rectangle R. cur_idx (>= 0) highlights a
// sample (the current animation frame); pass -1 for none.
void App::draw_series(cairo_t *cr, const Rect &R,
                      const std::vector<double> &xv, const std::vector<double> &yv,
                      double ymin, double ymax, bool is_time,
                      const std::string &xunits, const std::string &xcal, int cur_idx) {
    const double ml = 64, mr = 16, mt = 10, mb = 34;
    double px0 = R.x + ml, py0 = R.y + mt, pw = R.w - ml - mr, ph = R.h - mt - mb;
    if (pw < 20 || ph < 20) return;
    if (ymax <= ymin) ymax = ymin + 1;

    int n = (int)yv.size();
    auto xmap = [&](int i) { return px0 + (n > 1 ? (double)i / (n - 1) : 0.5) * pw; };
    auto ymap = [&](double val) { return py0 + ph - (val - ymin) / (ymax - ymin) * ph; };

    // Plot frame.
    set_color(cr, COL_PLOTBG);
    cairo_rectangle(cr, px0, py0, pw, ph);
    cairo_fill(cr);

    // Y gridlines + labels.
    const int nyt = 5;
    cairo_set_line_width(cr, 1);
    for (int i = 0; i < nyt; ++i) {
        double f = (double)i / (nyt - 1);
        double val = ymax - f * (ymax - ymin);
        double y = py0 + f * ph;
        set_color(cr, COL_BORDER, 0.6);
        cairo_move_to(cr, px0, y); cairo_line_to(cr, px0 + pw, y);
        cairo_stroke(cr);
        double tw, th; text_size(cr, fmt_num(val), 11, false, tw, th);
        draw_text(cr, fmt_num(val), px0 - tw - 6, y - th / 2, COL_TEXT_DIM, 11);
    }

    // X ticks + labels.
    const int nxt = (n >= 6) ? 6 : std::max(2, n);
    for (int i = 0; i < nxt; ++i) {
        int idx = (nxt == 1) ? 0 : (int)std::lround((double)i / (nxt - 1) * (n - 1));
        double x = xmap(idx);
        set_color(cr, COL_BORDER, 0.4);
        cairo_move_to(cr, x, py0); cairo_line_to(cr, x, py0 + ph);
        cairo_stroke(cr);
        std::string lbl;
        if (is_time && idx < (int)xv.size())
            lbl = units_.format_time(xunits, xcal, xv[idx]);
        else if (idx < (int)xv.size())
            lbl = fmt_num(xv[idx]);
        else
            lbl = std::to_string(idx);
        double tw, th; text_size(cr, lbl, 10, false, tw, th);
        double tx = std::clamp(x - tw / 2, R.x + 2, R.x + R.w - tw - 2);
        draw_text(cr, lbl, tx, py0 + ph + 6, COL_TEXT_DIM, 10);
    }

    // Highlight a current sample.
    if (cur_idx >= 0 && cur_idx < n) {
        double x = xmap(cur_idx);
        set_color(cr, COL_ACCENT, 0.5);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, x, py0); cairo_line_to(cr, x, py0 + ph);
        cairo_stroke(cr);
    }

    // Axis border.
    set_color(cr, COL_BORDER);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, px0, py0, pw, ph);
    cairo_stroke(cr);

    // The series line (break across missing values).
    set_color(cr, COL_ACCENT);
    cairo_set_line_width(cr, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    bool pen = false;
    for (int i = 0; i < n; ++i) {
        if (std::isnan(yv[i])) { pen = false; continue; }
        double x = xmap(i), y = ymap(yv[i]);
        if (pen) cairo_line_to(cr, x, y); else cairo_move_to(cr, x, y);
        pen = true;
    }
    cairo_stroke(cr);

    // Point markers when the series is short enough to be legible.
    if (n <= 80) {
        for (int i = 0; i < n; ++i) {
            if (std::isnan(yv[i])) continue;
            double x = xmap(i), y = ymap(yv[i]);
            set_color(cr, i == cur_idx ? RGB{1, 1, 1} : COL_ACCENT);
            cairo_arc(cr, x, y, i == cur_idx ? 4 : 2.5, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
}

// The main-window line plot for a 1-D variable.
void App::draw_plot_line() {
    draw_series(cr_, r_plot_, line_x_, line_vals_, line_ymin_, line_ymax_,
                line_is_time_, line_xunits_, line_xcal_, -1);
}

// ---- main loop -------------------------------------------------------------

int App::run() {
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) {
        std::fprintf(stderr, "ncvista: cannot open X display (is $DISPLAY set?)\n");
        return 1;
    }
    int screen = DefaultScreen(dpy_);
    Window root = RootWindow(dpy_, screen);

    apply_default_size();              // default to a global lon/lat grid

    XSetWindowAttributes attrs;
    attrs.background_pixel = BlackPixel(dpy_, screen);
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                       ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
    win_ = XCreateWindow(dpy_, root, 0, 0, width_, height_, 0,
                         DefaultDepth(dpy_, screen), InputOutput,
                         DefaultVisual(dpy_, screen),
                         CWBackPixel | CWEventMask, &attrs);

    std::string wname = "ncvista " NCVISTA_VERSION " — " + nc_.path();
    XStoreName(dpy_, win_,
               ("ncvista " NCVISTA_VERSION " - " + nc_.path()).c_str()); // ASCII fallback
    // Proper UTF-8 title for modern window managers (_NET_WM_NAME).
    Atom net_name = XInternAtom(dpy_, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy_, "UTF8_STRING", False);
    XChangeProperty(dpy_, win_, net_name, utf8, 8, PropModeReplace,
                    (const unsigned char *)wname.c_str(), (int)wname.size());
    XClassHint *ch = XAllocClassHint();
    ch->res_name = (char *)"ncvista"; ch->res_class = (char *)"Ncvista";
    XSetClassHint(dpy_, win_, ch); XFree(ch);

    wm_delete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy_, win_, &wm_delete_, 1);

    XMapWindow(dpy_, win_);

    surf_ = cairo_xlib_surface_create(dpy_, win_, DefaultVisual(dpy_, screen),
                                      width_, height_);
    front_cr_ = cairo_create(surf_);
    make_back_buffer();

    int fd = ConnectionNumber(dpy_);
    bool running = true;
    while (running) {
        // drain events
        while (XPending(dpy_)) {
            XEvent ev;
            XNextEvent(dpy_, &ev);

            // Events for the metadata window are handled separately.
            if (meta_win_ && ev.xany.window == meta_win_) {
                switch (ev.type) {
                    case Expose:
                        if (ev.xexpose.count == 0) draw_meta();
                        break;
                    case ConfigureNotify:
                        if (ev.xconfigure.width != meta_w_ ||
                            ev.xconfigure.height != meta_h_) {
                            meta_w_ = ev.xconfigure.width;
                            meta_h_ = ev.xconfigure.height;
                            cairo_xlib_surface_set_size(meta_surf_, meta_w_, meta_h_);
                            scroll_meta(0); // re-clamp
                        }
                        draw_meta();
                        break;
                    case ButtonPress:
                        if (ev.xbutton.button == Button4) scroll_meta(-3 * 19);
                        else if (ev.xbutton.button == Button5) scroll_meta(3 * 19);
                        else if (ev.xbutton.button == Button1) {
                            meta_selecting_ = true;
                            meta_sel_text_.clear();   // starting a fresh selection
                            meta_point_from_mouse(ev.xbutton.x, ev.xbutton.y,
                                                  sel_a_line_, sel_a_col_);
                            sel_c_line_ = sel_a_line_; sel_c_col_ = sel_a_col_;
                        }
                        draw_meta();
                        break;
                    case MotionNotify:
                        if (meta_selecting_) {
                            // Coalesce queued motion for one redraw.
                            XEvent latest = ev;
                            while (XCheckTypedWindowEvent(dpy_, meta_win_, MotionNotify, &latest))
                                ;
                            meta_point_from_mouse(latest.xmotion.x, latest.xmotion.y,
                                                  sel_c_line_, sel_c_col_);
                            draw_meta();
                        }
                        break;
                    case ButtonRelease:
                        if (ev.xbutton.button == Button1 && meta_selecting_) {
                            meta_selecting_ = false;
                            meta_commit_selection();
                            draw_meta();
                        }
                        break;
                    case SelectionRequest: {
                        // Serve the current selection to a requesting client.
                        XSelectionRequestEvent *rq = &ev.xselectionrequest;
                        XSelectionEvent resp{};
                        resp.type = SelectionNotify;
                        resp.display = rq->display;
                        resp.requestor = rq->requestor;
                        resp.selection = rq->selection;
                        resp.target = rq->target;
                        resp.time = rq->time;
                        resp.property = rq->property;
                        if (rq->target == utf8_atom_ || rq->target == XA_STRING) {
                            XChangeProperty(dpy_, rq->requestor, rq->property, rq->target,
                                            8, PropModeReplace,
                                            (const unsigned char *)meta_sel_text_.data(),
                                            (int)meta_sel_text_.size());
                        } else if (rq->target == targets_atom_) {
                            Atom targs[] = {targets_atom_, utf8_atom_, XA_STRING};
                            XChangeProperty(dpy_, rq->requestor, rq->property, XA_ATOM,
                                            32, PropModeReplace,
                                            (const unsigned char *)targs, 3);
                        } else {
                            resp.property = None;  // unsupported target
                        }
                        XSendEvent(dpy_, rq->requestor, False, 0, (XEvent *)&resp);
                        break;
                    }
                    case SelectionClear:
                        // Another client took the selection: drop our highlight.
                        meta_sel_text_.clear();
                        draw_meta();
                        break;
                    case KeyPress: {
                        KeySym mk = XLookupKeysym(&ev.xkey, 0);
                        if (mk == XK_q || mk == XK_Escape || mk == XK_m || mk == XK_i)
                            close_meta_window();
                        else if (mk == XK_Up) { scroll_meta(-19); draw_meta(); }
                        else if (mk == XK_Down) { scroll_meta(19); draw_meta(); }
                        else if (mk == XK_Prior) { scroll_meta(-meta_h_ + 40); draw_meta(); }
                        else if (mk == XK_Next || mk == XK_space) { scroll_meta(meta_h_ - 40); draw_meta(); }
                        else if (mk == XK_Home) { meta_scroll_ = 0; draw_meta(); }
                        break;
                    }
                    case ClientMessage:
                        if ((Atom)ev.xclient.data.l[0] == wm_delete_) close_meta_window();
                        break;
                }
                continue;
            }

            // Events for the time-series window.
            if (ts_win_ && ev.xany.window == ts_win_) {
                switch (ev.type) {
                    case Expose:
                        if (ev.xexpose.count == 0) draw_ts();
                        break;
                    case ConfigureNotify:
                        if (ev.xconfigure.width != ts_w_ ||
                            ev.xconfigure.height != ts_h_) {
                            ts_w_ = ev.xconfigure.width;
                            ts_h_ = ev.xconfigure.height;
                            cairo_xlib_surface_set_size(ts_surf_, ts_w_, ts_h_);
                        }
                        draw_ts();
                        break;
                    case KeyPress: {
                        KeySym tk = XLookupKeysym(&ev.xkey, 0);
                        if (tk == XK_q || tk == XK_Escape) close_ts_window();
                        break;
                    }
                    case ClientMessage:
                        if ((Atom)ev.xclient.data.l[0] == wm_delete_) close_ts_window();
                        break;
                }
                continue;
            }

            switch (ev.type) {
                case Expose:
                    if (ev.xexpose.count == 0) render();
                    break;
                case ConfigureNotify: {
                    // Coalesce the burst of resize events from a drag: act only
                    // on the final geometry, so we reallocate/redraw once.
                    XEvent latest = ev;
                    while (XCheckTypedWindowEvent(dpy_, win_, ConfigureNotify, &latest))
                        ;
                    if (latest.xconfigure.width != width_ ||
                        latest.xconfigure.height != height_) {
                        width_ = latest.xconfigure.width;
                        height_ = latest.xconfigure.height;
                        cairo_xlib_surface_set_size(surf_, width_, height_);
                        make_back_buffer();   // resize back buffer to match
                        render();
                    }
                    break;
                }
                case ButtonPress:
                    if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
                        int dir = (ev.xbutton.button == Button4) ? -1 : 1;
                        // Wheel over the variable list scrolls it; over a zoomed
                        // plot it pans (Shift = horizontal); elsewhere it adjusts
                        // the animation rate.
                        if (r_sidebar_.hit(ev.xbutton.x, ev.xbutton.y)) {
                            scroll_sidebar(dir * 57);
                        } else if (zoomed_ && r_plot_.hit(ev.xbutton.x, ev.xbutton.y)) {
                            if (ev.xbutton.state & ShiftMask)
                                pan_zoom(dir * 0.15 * (zoom_fx1_ - zoom_fx0_), 0.0);
                            else
                                pan_zoom(0.0, dir * 0.15 * (zoom_fy1_ - zoom_fy0_));
                        } else {
                            fps_ = std::clamp(fps_ - dir, 1.0, 30.0);
                        }
                    } else {
                        on_button(ev.xbutton.x, ev.xbutton.y, ev.xbutton.button);
                    }
                    render();
                    break;
                case ButtonRelease:
                    drag_slider_ = -1;
                    drag_sidebar_ = false;
                    plot_sb_drag_ = 0;
                    if (ev.xbutton.button == Button1 && selecting_) {
                        end_selection(ev.xbutton.x, ev.xbutton.y);
                        render();
                    }
                    break;
                case MotionNotify: {
                    // Coalesce: keep only the most recent queued motion event so
                    // a fast move triggers one redraw, not dozens.
                    XEvent latest = ev;
                    while (XCheckTypedWindowEvent(dpy_, win_, MotionNotify, &latest))
                        ;
                    on_motion(latest.xmotion.x, latest.xmotion.y);
                    render();
                    break;
                }
                case KeyPress: {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (editing_) {            // typing into a bound field
                        handle_edit_key(ev.xkey, ks);
                        render();
                        break;
                    }
                    if (ks == XK_q || ks == XK_Escape) { running = false; break; }
                    on_key(ks);
                    render();
                    break;
                }
                case ClientMessage:
                    if ((Atom)ev.xclient.data.l[0] == wm_delete_) running = false;
                    break;
            }
        }
        if (!running) break;

        // animation timing via select() timeout
        struct timeval tv;
        if (playing_) {
            double interval = 1.0 / fps_;
            double t = now_seconds();
            double wait = last_frame_ + interval - t;
            if (wait <= 0) {
                step_anim(+1);
                last_frame_ = now_seconds();
                render();
                wait = interval;
            }
            tv.tv_sec = 0;
            tv.tv_usec = (suseconds_t)(std::max(0.0, std::min(wait, interval)) * 1e6);
        } else {
            tv.tv_sec = 1; tv.tv_usec = 0;
        }
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        select(fd + 1, &fds, nullptr, nullptr, &tv);
    }

    close_meta_window();
    close_ts_window();
    if (data_img_) cairo_surface_destroy(data_img_);
    if (coast_ov_.img) cairo_surface_destroy(coast_ov_.img);
    if (borders_ov_.img) cairo_surface_destroy(borders_ov_.img);
    cairo_destroy(cr_);
    cairo_surface_destroy(back_);
    cairo_destroy(front_cr_);
    cairo_surface_destroy(surf_);
    XDestroyWindow(dpy_, win_);
    XCloseDisplay(dpy_);
    return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "--version") == 0 ||
                      std::strcmp(argv[1], "-v") == 0)) {
        std::printf("ncvista %s\n", NCVISTA_VERSION);
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr,
            "ncvista %s — a modern netCDF viewer\n"
            "usage: %s FILE.nc\n\n"
            "keys: space play/pause   ←/→ step time   Home first step\n"
            "      ↑/↓ change variable\n"
            "      c colormap   f flip-Y   a auto/fixed range   l coastlines\n"
            "      m metadata   +/- speed   q quit\n"
            "      (click a colorbar min/max field to type a fixed bound)\n"
            "      (click a grid cell to plot its time series)\n",
            NCVISTA_VERSION, argv[0]);
        return 2;
    }

    NcFile nc;
    std::string err;
    if (!nc.open(argv[1], err)) {
        std::fprintf(stderr, "ncvista: %s\n", err.c_str());
        return 1;
    }
    if (nc.displayable().empty()) {
        std::fprintf(stderr, "ncvista: no displayable (numeric) variables in %s\n",
                     argv[1]);
        return 1;
    }

    Units units;
    if (!units.ok())
        std::fprintf(stderr, "ncvista: warning: udunits database not found; "
                             "time axes will show raw numbers.\n");

    App app(nc, units);
    return app.run();
}
