[PART 1/5]

/*
 * battery-info — sysfs-based battery monitor (MediaTek/Android compatible)
 *
 * Replaces UPower backend with direct sysfs probing.
 */

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ────────────────────────────────────────────────
 *  Constants
 * ──────────────────────────────────────────────── */

#define EVENT_LOG_HEIGHT   10
#define MAX_EVENTS         256
#define BAR_WIDTH          28
#define MAX_THRESHOLDS     16

#define URGENCY_LOW      ((guchar)0)
#define URGENCY_NORMAL   ((guchar)1)
#define URGENCY_CRITICAL ((guchar)2)

/* ────────────────────────────────────────────────
 *  Battery state enum (kept from original)
 * ──────────────────────────────────────────────── */

typedef enum {
    STATE_UNKNOWN           = 0,
    STATE_CHARGING          = 1,
    STATE_DISCHARGING       = 2,
    STATE_EMPTY             = 3,
    STATE_FULLY_CHARGED     = 4,
    STATE_PENDING_CHARGE    = 5,
    STATE_PENDING_DISCHARGE = 6,
} BatteryState;

/* ────────────────────────────────────────────────
 *  Event log
 * ──────────────────────────────────────────────── */

typedef enum {
    EVT_INFO,
    EVT_PLUG,
    EVT_UNPLUG,
    EVT_STATE,
    EVT_MILESTONE,
    EVT_ALERT,
} EventKind;

typedef struct {
    char      ts[12];
    char      msg[128];
    EventKind kind;
} Event;

static Event g_events[MAX_EVENTS];
static int   g_event_head  = 0;
static int   g_event_count = 0;

static void event_push(EventKind kind, const char *fmt, ...)
{
    Event *ev = &g_events[(g_event_head + g_event_count) % MAX_EVENTS];

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(ev->ts, sizeof ev->ts, "%H:%M:%S", tm);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev->msg, sizeof ev->msg, fmt, ap);
    va_end(ap);

    ev->kind = kind;

    if (g_event_count < MAX_EVENTS)
        g_event_count++;
    else
        g_event_head = (g_event_head + 1) % MAX_EVENTS;
}

/* ────────────────────────────────────────────────
 *  Sysfs battery presence detection
 * ──────────────────────────────────────────────── */

static gboolean sysfs_battery_exists(void)
{
    const char *paths[] = {
        "/sys/class/power_supply/battery",
        "/sys/devices/platform/battery",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        if (g_file_test(paths[i], G_FILE_TEST_IS_DIR))
            return TRUE;
    }
    return FALSE;
}

/* ────────────────────────────────────────────────
 *  Battery data struct
 * ──────────────────────────────────────────────── */

typedef struct {
    gdouble  percentage;
    gdouble  voltage;
    gdouble  current_now;
    gdouble  current_avg;
    gdouble  temperature;
    gdouble  energy_rate;

    gint32   cycle_count;
    gint64   update_time;

    guint32  state;
    gboolean is_present;
    gboolean is_rechargeable;
} BatData;

[PART 2/5]

/* ────────────────────────────────────────────────
 *  Sysfs battery reader (MediaTek/Android compatible)
 * ──────────────────────────────────────────────── */
static void bat_data_read_sysfs(BatData *d)
{
    memset(d, 0, sizeof *d);
    d->cycle_count = -1;

    gchar *s = NULL;

    /* Percentage */
    if (g_file_get_contents("/sys/class/power_supply/battery/capacity",
                            &s, NULL, NULL)) {
        d->percentage = g_ascii_strtod(g_strstrip(s), NULL);
        g_free(s); s = NULL;
    }

    /* Charging state */
    if (g_file_get_contents("/sys/class/power_supply/battery/status",
                            &s, NULL, NULL)) {

        if (g_str_has_prefix(s, "Charging"))
            d->state = STATE_CHARGING;
        else if (g_str_has_prefix(s, "Discharging"))
            d->state = STATE_DISCHARGING;
        else if (g_str_has_prefix(s, "Full"))
            d->state = STATE_FULLY_CHARGED;
        else
            d->state = STATE_UNKNOWN;

        g_free(s); s = NULL;
    }

    /* Voltage (µV → V) */
    if (g_file_get_contents("/sys/class/power_supply/battery/voltage_now",
                            &s, NULL, NULL)) {
        d->voltage = g_ascii_strtod(g_strstrip(s), NULL) / 1e6;
        g_free(s); s = NULL;
    }

    /* Current (µA → A) */
    if (g_file_get_contents("/sys/class/power_supply/battery/current_now",
                            &s, NULL, NULL)) {
        d->current_now = g_ascii_strtod(g_strstrip(s), NULL) / 1e6;
        g_free(s); s = NULL;
    }

    /* Average current (optional) */
    if (g_file_get_contents("/sys/class/power_supply/battery/current_avg",
                            &s, NULL, NULL)) {
        d->current_avg = g_ascii_strtod(g_strstrip(s), NULL) / 1e6;
        g_free(s); s = NULL;
    }

    /* Temperature (tenths of °C → °C) */
    if (g_file_get_contents("/sys/class/power_supply/battery/temp",
                            &s, NULL, NULL)) {
        d->temperature = g_ascii_strtod(g_strstrip(s), NULL) / 10.0;
        g_free(s); s = NULL;
    }

    /* Cycle count */
    if (g_file_get_contents("/sys/class/power_supply/battery/cycle_count",
                            &s, NULL, NULL)) {
        d->cycle_count = (gint32)g_ascii_strtoll(g_strstrip(s), NULL, 10);
        g_free(s); s = NULL;
    }

    /* Compute energy rate if possible */
    if (d->voltage > 0 && d->current_now > 0)
        d->energy_rate = d->voltage * d->current_now;

    d->is_present = TRUE;
    d->is_rechargeable = TRUE;

    /* Timestamp */
    d->update_time = (gint64)time(NULL);
}

[PART 3/5]

/* ────────────────────────────────────────────────
 *  Previous-state tracker (UPower-free)
 * ──────────────────────────────────────────────── */

typedef struct {
    guint32  battery_state;
    int      pct_milestone;
    gboolean charger_online;
    gboolean initialized;
    gboolean notified_full;
} PrevState;

static PrevState g_prev = { 0 };

static void prev_state_init(void)
{
    g_prev.battery_state = (guint32)-1;
    g_prev.pct_milestone = -1;
    g_prev.charger_online = FALSE;
    g_prev.initialized = TRUE;
    g_prev.notified_full = FALSE;
}

/* ────────────────────────────────────────────────
 *  Charger detection via sysfs
 * ──────────────────────────────────────────────── */

static gboolean sysfs_charger_online(void)
{
    gchar *s = NULL;

    /* AC charger */
    if (g_file_get_contents("/sys/class/power_supply/ac/online",
                            &s, NULL, NULL)) {
        gboolean on = (g_strstrip(s)[0] == '1');
        g_free(s);
        return on;
    }

    /* USB charger */
    if (g_file_get_contents("/sys/class/power_supply/usb/online",
                            &s, NULL, NULL)) {
        gboolean on = (g_strstrip(s)[0] == '1');
        g_free(s);
        return on;
    }

    /* Fallback: infer from battery status */
    if (g_file_get_contents("/sys/class/power_supply/battery/status",
                            &s, NULL, NULL)) {
        gboolean on = g_str_has_prefix(s, "Charging") ||
                      g_str_has_prefix(s, "Full");
        g_free(s);
        return on;
    }

    return FALSE;
}

/* ────────────────────────────────────────────────
 *  Detect changes (state, charger, milestones)
 * ──────────────────────────────────────────────── */

static void detect_changes(const BatData *d)
{
    if (!g_prev.initialized)
        return;

    /* ── Battery state transition ───────────────── */
    if (g_prev.battery_state != (guint32)-1 &&
        g_prev.battery_state != d->state) {

        event_push(EVT_STATE, "State: %s → %s",
                   (g_prev.battery_state == STATE_CHARGING) ? "Charging" :
                   (g_prev.battery_state == STATE_DISCHARGING) ? "Discharging" :
                   (g_prev.battery_state == STATE_FULLY_CHARGED) ? "Full" :
                   "Unknown",
                   (d->state == STATE_CHARGING) ? "Charging" :
                   (d->state == STATE_DISCHARGING) ? "Discharging" :
                   (d->state == STATE_FULLY_CHARGED) ? "Full" :
                   "Unknown");

        /* Full-charge notification */
        if (d->state == STATE_FULLY_CHARGED && !g_prev.notified_full) {
            g_prev.notified_full = TRUE;
            event_push(EVT_INFO, "🔋 Battery fully charged");
        }

        if (d->state == STATE_DISCHARGING)
            g_prev.notified_full = FALSE;
    }

    g_prev.battery_state = d->state;

    /* ── 10% milestones ─────────────────────────── */
    int milestone = (int)(d->percentage / 10.0) * 10;

    if (g_prev.pct_milestone >= 0 && milestone != g_prev.pct_milestone) {
        EventKind k = (milestone <= 20) ? EVT_ALERT : EVT_INFO;
        event_push(k, "Reached %d%% (%s)",
                   milestone,
                   (d->state == STATE_CHARGING) ? "Charging" :
                   (d->state == STATE_DISCHARGING) ? "Discharging" :
                   "Unknown");
    }

    g_prev.pct_milestone = milestone;

    /* ── Charger plug/unplug detection ───────────── */
    gboolean now_online = sysfs_charger_online();

    if (g_prev.charger_online != now_online) {
        if (now_online)
            event_push(EVT_PLUG, "Charger connected");
        else
            event_push(EVT_UNPLUG, "Charger disconnected");

        g_prev.charger_online = now_online;
    }
}

[PART 4/5]

/* ────────────────────────────────────────────────
 *  Ncurses colour setup
 * ──────────────────────────────────────────────── */

enum {
    CP_HEADER     = 1,
    CP_SECTION    = 2,
    CP_LABEL      = 3,
    CP_GOOD       = 4,
    CP_WARN       = 5,
    CP_BAD        = 6,
    CP_DIM        = 7,
    CP_BAR_FILL   = 8,
    CP_BAR_EMPTY  = 9,
    CP_EVT_TIME   = 10,
    CP_EVT_PLUG   = 11,
    CP_EVT_UNPLUG = 12,
    CP_EVT_STATE  = 13,
    CP_EVT_INFO   = 14,
    CP_EVT_ALERT  = 15,
};

static void nc_init_colors(void)
{
    start_color();
    use_default_colors();

    init_pair(CP_HEADER,     COLOR_CYAN,   -1);
    init_pair(CP_SECTION,    COLOR_WHITE,  -1);
    init_pair(CP_LABEL,      COLOR_WHITE,  -1);
    init_pair(CP_GOOD,       COLOR_GREEN,  -1);
    init_pair(CP_WARN,       COLOR_YELLOW, -1);
    init_pair(CP_BAD,        COLOR_RED,    -1);
    init_pair(CP_DIM,        COLOR_WHITE,  -1);
    init_pair(CP_BAR_FILL,   COLOR_GREEN,  -1);
    init_pair(CP_BAR_EMPTY,  COLOR_WHITE,  -1);

    init_pair(CP_EVT_TIME,   COLOR_CYAN,   -1);
    init_pair(CP_EVT_PLUG,   COLOR_GREEN,  -1);
    init_pair(CP_EVT_UNPLUG, COLOR_RED,    -1);
    init_pair(CP_EVT_STATE,  COLOR_YELLOW, -1);
    init_pair(CP_EVT_INFO,   COLOR_WHITE,  -1);
    init_pair(CP_EVT_ALERT,  COLOR_RED,    -1);
}

/* ────────────────────────────────────────────────
 *  UI helpers
 * ──────────────────────────────────────────────── */

static const char *state_str(guint32 s)
{
    switch (s) {
    case STATE_CHARGING:      return "Charging";
    case STATE_DISCHARGING:   return "Discharging";
    case STATE_FULLY_CHARGED: return "Full";
    default:                  return "Unknown";
    }
}

static void draw_titlebar(WINDOW *w, const char *left, const char *right)
{
    int cols = getmaxx(w);

    wattron(w, COLOR_PAIR(CP_HEADER) | A_BOLD | A_REVERSE);
    wmove(w, 0, 0);

    for (int i = 0; i < cols; i++)
        waddch(w, ' ');

    mvwprintw(w, 0, 1, "%s", left);

    int rlen = strlen(right);
    if (cols - rlen - 1 > (int)strlen(left) + 2)
        mvwprintw(w, 0, cols - rlen - 1, "%s", right);

    wattroff(w, COLOR_PAIR(CP_HEADER) | A_BOLD | A_REVERSE);
}

static void section(WINDOW *w, int *y, const char *title)
{
    wattron(w, COLOR_PAIR(CP_SECTION) | A_BOLD);
    mvwprintw(w, (*y)++, 2, "── %s ──", title);
    wattroff(w, COLOR_PAIR(CP_SECTION) | A_BOLD);
}

#define LCOL 22

static void field(WINDOW *w, int *y, int cols,
                  int lcp, const char *label,
                  int vcp, int vattr,
                  const char *fmt, ...)
{
    if (*y >= getmaxy(w) - 1)
        return;

    char vbuf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vbuf, sizeof vbuf, fmt, ap);
    va_end(ap);

    int avail = cols - LCOL - 4;
    if (avail > 0 && (int)strlen(vbuf) > avail)
        vbuf[avail] = '\0';

    wattron(w, COLOR_PAIR(lcp));
    mvwprintw(w, *y, 2, "%-*s", LCOL, label);
    wattroff(w, COLOR_PAIR(lcp));

    wattron(w, COLOR_PAIR(vcp) | vattr);
    wprintw(w, "%s", vbuf);
    wattroff(w, COLOR_PAIR(vcp) | vattr);

    (*y)++;
}

static void draw_bar(WINDOW *w, int y, double frac, guint32 state)
{
    int cols = getmaxx(w);
    int bw   = MIN(BAR_WIDTH, cols - 16);

    if (bw < 4)
        return;

    int filled = (int)(frac * bw + 0.5);

    int bar_cp =
        (state == STATE_CHARGING || state == STATE_FULLY_CHARGED)
        ? CP_GOOD
        : (frac < 0.2) ? CP_BAD : CP_WARN;

    wmove(w, y, 2);

    wattron(w, COLOR_PAIR(bar_cp) | A_BOLD);
    waddch(w, '[');

    for (int i = 0; i < bw; i++) {
        if (i < filled) {
            waddch(w, '#');
        } else {
            wattroff(w, COLOR_PAIR(bar_cp) | A_BOLD);
            wattron(w, COLOR_PAIR(CP_BAR_EMPTY) | A_DIM);
            waddch(w, '-');
            wattroff(w, COLOR_PAIR(CP_BAR_EMPTY) | A_DIM);
            wattron(w, COLOR_PAIR(bar_cp) | A_BOLD);
        }
    }

    waddch(w, ']');
    wattroff(w, COLOR_PAIR(bar_cp) | A_BOLD);

    wattron(w, COLOR_PAIR(bar_cp) | A_BOLD);
    wprintw(w, "  %5.1f%%", frac * 100.0);
    wattroff(w, COLOR_PAIR(bar_cp) | A_BOLD);

    const char *badge = state_str(state);
    int badge_x = 2 + bw + 2 + 8;

    if (badge_x + strlen(badge) + 3 < cols) {
        wattron(w, COLOR_PAIR(bar_cp));
        mvwprintw(w, y, badge_x, "[%s]", badge);
        wattroff(w, COLOR_PAIR(bar_cp));
    }
}

/* ────────────────────────────────────────────────
 *  Render stats panel (sysfs version)
 * ──────────────────────────────────────────────── */

static void render_stats(WINDOW *w, gboolean watch_mode)
{
    BatData d;
    bat_data_read_sysfs(&d);

    if (watch_mode)
        detect_changes(&d);

    werase(w);

    int cols = getmaxx(w);
    int y = 0;

    time_t now = time(NULL);
    char ts[12];
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));

    char left[80];
    snprintf(left, sizeof left,
             "  Battery Monitor%s",
             watch_mode ? "  [Ctrl+C to quit]" : "");

    draw_titlebar(w, left, ts);
    y++;

    y++;
    draw_bar(w, y++, d.percentage / 100.0, d.state);
    y++;

    section(w, &y, "Charge");

    field(w, &y, cols, CP_LABEL, "Capacity",
          (d.percentage < 20.0) ? CP_BAD :
          (d.percentage < 40.0) ? CP_WARN : CP_GOOD,
          A_BOLD, "%.1f %%", d.percentage);

    field(w, &y, cols, CP_LABEL, "State",
          CP_LABEL, 0, "%s", state_str(d.state));

    field(w, &y, cols, CP_LABEL, "Voltage",
          CP_LABEL, 0, "%.3f V", d.voltage);

    field(w, &y, cols, CP_LABEL, "Current",
          CP_LABEL, 0, "%.3f A", d.current_now);

    field(w, &y, cols, CP_LABEL, "Temperature",
          CP_LABEL, 0, "%.1f °C", d.temperature);

    if (d.cycle_count >= 0)
        field(w, &y, cols, CP_LABEL, "Cycle Count",
              CP_LABEL, 0, "%d", d.cycle_count);

    wrefresh(w);
}

[PART 5/5]

/* ────────────────────────────────────────────────
 *  Event log window renderer
 * ──────────────────────────────────────────────── */

static void render_events(WINDOW *w)
{
    werase(w);

    int rows = getmaxy(w);
    int start = MAX(0, g_event_count - rows);

    for (int i = 0; i < rows && (start + i) < g_event_count; i++) {
        Event *ev = &g_events[(g_event_head + start + i) % MAX_EVENTS];

        int cp =
            (ev->kind == EVT_PLUG)   ? CP_EVT_PLUG :
            (ev->kind == EVT_UNPLUG) ? CP_EVT_UNPLUG :
            (ev->kind == EVT_STATE)  ? CP_EVT_STATE :
            (ev->kind == EVT_ALERT)  ? CP_EVT_ALERT :
            (ev->kind == EVT_INFO)   ? CP_EVT_INFO :
                                       CP_EVT_INFO;

        wattron(w, COLOR_PAIR(CP_EVT_TIME));
        mvwprintw(w, i, 1, "%s", ev->ts);
        wattroff(w, COLOR_PAIR(CP_EVT_TIME));

        wattron(w, COLOR_PAIR(cp));
        mvwprintw(w, i, 11, "%s", ev->msg);
        wattroff(w, COLOR_PAIR(cp));
    }

    wrefresh(w);
}

/* ────────────────────────────────────────────────
 *  Main program loop
 * ──────────────────────────────────────────────── */

static gboolean tick_cb(gpointer data)
{
    WINDOW **wins = data;

    render_stats(wins[0], TRUE);
    render_events(wins[1]);

    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    gboolean watch_mode = FALSE;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--watch") == 0)
            watch_mode = TRUE;
    }

    /* Ensure sysfs battery exists */
    if (!sysfs_battery_exists()) {
        g_printerr("No battery device found.\n");
        return 1;
    }

    /* Initialize previous-state tracker */
    prev_state_init();

    /* ────────────────────────────────────────────────
     *  One-shot mode
     * ──────────────────────────────────────────────── */
    if (!watch_mode) {
        BatData d;
        bat_data_read_sysfs(&d);

        printf("Battery: %.1f%%, %s\n",
               d.percentage,
               state_str(d.state));

        printf("Voltage: %.3f V\n", d.voltage);
        printf("Current: %.3f A\n", d.current_now);
        printf("Temp:    %.1f °C\n", d.temperature);

        if (d.cycle_count >= 0)
            printf("Cycles:  %d\n", d.cycle_count);

        return 0;
    }

    /* ────────────────────────────────────────────────
     *  Watch mode (ncurses UI)
     * ──────────────────────────────────────────────── */

    initscr();
    cbreak();
    noecho();
    curs_set(0);

    nc_init_colors();

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int stats_h = rows - EVENT_LOG_HEIGHT;
    int events_h = EVENT_LOG_HEIGHT;

    WINDOW *stats_win  = newwin(stats_h, cols, 0, 0);
    WINDOW *events_win = newwin(events_h, cols, stats_h, 0);

    WINDOW *wins[2] = { stats_win, events_win };

    /* First render */
    render_stats(stats_win, TRUE);
    render_events(events_win);

    /* GLib main loop for periodic updates */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    g_timeout_add(1000, tick_cb, wins);

    /* Handle Ctrl+C */
    g_unix_signal_add(SIGINT, (GSourceFunc)g_main_loop_quit, loop);

    g_main_loop_run(loop);

    /* Cleanup */
    delwin(stats_win);
    delwin(events_win);
    endwin();

    return 0;
}

