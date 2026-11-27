

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/swap.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sched.h>
#include <poll.h>
#include <math.h>

#define LMK_VERSION   "1.0"
#define LMK_AUTHOR    "@fuckyoustan & P34"

/* Memory pressure thresholds */
#define FREE_HIGH_PCT    25   /* stop killing above this % avail   */
#define FREE_LOW_PCT     15   /* start killing below this %        */
#define FREE_CRIT_PCT     8   /* aggressive kill tier              */
#define PIN_ALLOW_FLOOR_PCT 4

/* ZRAM settings */
#define ZRAM_SIZE_PCT    50

#define ZRAM_WARN_PCT    90   /* early-warning: proactive page drop    */
#define ZRAM_TRIM_PCT    93   /* pressure kill tier                    */
#define ZRAM_CRIT_PCT    98   /* try full cycle                        */

/* Minimum rate at which zram_pressure_kill() is called (seconds). */
#define ZRAM_KILL_INTVL_S    5

/* Minimum avail buffer (kB) above orig_data_size to attempt a cycle. */
#define ZRAM_CYCLE_MARGIN_KB  (200 * 1024)   /* 200 MB safety buffer   */

/* Maximum ZRAM pressure kills per invocation of zram_pressure_kill(). */
#define ZRAM_KILL_BATCH       6

/* ZRAM stuck / deep clean */
#define ZRAM_STUCK_PCT            96

#define ZRAM_STUCK_S              30
#define ZRAM_DEEPCLEAN_CD_S      180   /* min seconds between deep cleans */
#define ZRAM_COMPACT_WAIT_MS    1500   /* ms to wait after zram compact   */

#define ZRAM_CYCLE_RETRIES           8
#define ZRAM_CYCLE_RETRY_BACKOFF_US  150000

#define DEEPCLEAN_KILL_MAX         24   /* max procs killed per deep clean */
#define DEEPCLEAN_KILL_TARGET_PCT  25   /* stop once this % of ZRAM freed  */

/* Emergency kill: when ZRAM >= STUCK and every candidate is on cooldown, a second pass uses this reduced cooldown floor (seconds). */
#define ZRAM_EMERG_CD_S           10
#define ZRAM_EMERG_KILL_MAX        2

#define BOUNCE_SUPPRESS_RC        20        /* rc >= this -> suppress ZRAM kill */
#define BOUNCE_SUPPRESS_SWAP_KB   (150*1024) /* override: >150 MB swap = kill  */
#define BOUNCE_WINDOW_S           60        /* time window for bounce count    */
#define BOUNCE_WINDOW_KILLS       3         /* >3 kills in window -> suppress   */
#define BOUNCE_DECAY_S            (24*60*60) /* 24h since last bounce -> reset rc */

#define LOWFREQ_MIN_OBS_S         (24*60*60) /* don't judge until tracked 24h+  */
#define LOWFREQ_FG_RATE_THRESH    0.5        /* fg-credited ticks per hour tracked;
                                               * below this -> background candidate */

/* Deep-clean futility detection */
#define DEEPCLEAN_FUTILE_PCT       2    /* <=2% recovered = futile            */
#define DEEPCLEAN_FUTILE_STRIKES   1
#define DEEPCLEAN_FUTILE_PAUSE_S  300

/* Adaptive clean (tiered, trend-aware) */
#define ADAPTIVE_INTVL_LOW_S      45   /* interval when ZRAM mildly elevated  */
#define ADAPTIVE_INTVL_MED_S      25   /* interval when ZRAM clearly rising   */
#define ADAPTIVE_INTVL_HIGH_S     12   /* interval under heavy pressure       */
#define ADAPTIVE_KILLS_LOW         3   /* max kills at low tier               */
#define ADAPTIVE_KILLS_MED         7   /* max kills at medium tier            */
#define ADAPTIVE_KILLS_HIGH       12   /* max kills at high tier              */
#define ADAPTIVE_TARGET_LOW_PCT   5    /* free 5% of ZRAM disksize            */
#define ADAPTIVE_TARGET_MED_PCT   10   /* free 10%                            */
#define ADAPTIVE_TARGET_HIGH_PCT  25   /* free 25%                            */
#define ADAPTIVE_TREND_WINDOW_S   60   /* seconds to measure ZRAM rise        */
#define ADAPTIVE_RISE_THRESH_PCT   4   /* pct rise over window = "rising"     */
#define ADAPTIVE_INTVL_MAINT_S   120   /* maintenance kill interval (normal)  */
#define ADAPTIVE_KILLS_MAINT       1   /* only 1 stale kill at maintenance    */

/* Rank cache: avg_swap_kb and rank are saved every RANK_CACHE_SAVE_S and loaded at startup, so cmp_killable_zram targets high-swap apps right after a restart. */
#define RANK_CACHE_FILE  "/data/local/tmp/lmk_rank.cache"
#define RANK_CACHE_SAVE_S  10

/* Idle mode: after IDLE_DETECT_S without a foreground change, run a gentle COLD+STALE sweep (up to IDLE_KILLS_MAX every IDLE_INTVL_S), only when RAM isn't under pressure. */
#define IDLE_DETECT_S     300   /* seconds of no fg change = idle      */
#define IDLE_INTVL_S      120
#define IDLE_KILLS_MAX    2
#define IDLE_MIN_ZRAM_FREE_PCT 10
/* Age-based kill gates within IDLE tier */
#define IDLE_KILL_HOT_AGE_S    (14*60)
#define IDLE_PROTECT_PCT        50

/* Intense idle clean: device screen-off >=30 min, best opportunity to
 * sweep harder and run compaction since nothing is user-visible. */
#define IDLE_DEEP_S            1800  /* 30 min continuous screen-off       */
#define IDLE_DEEP_KILLS_MAX    8     /* generous kill budget                */
#define IDLE_DEEP_INTVL_S      300   /* run at most every 5 min             */
#define IDLE_DEEP_TARGET_PCT   70
#define IDLE_DEEP_HOT_AGE_S    60    /* relaxed age gate, already idle 30m */
#define IDLE_DEEP_PROTECT_PCT   20

#define LOG_RATELIMIT_S           30   /* noisy-state logs: once per 30 s    */

#define RANK_EXEMPT   0
#define RANK_TIER1    1   /* was FOREGROUND+HOT */
#define RANK_TIER2    2   /* was WARM+COLD */
#define RANK_TIER3    3   /* was STALE */
#define RANK_TIER4    4   /* was BACKGROUND */

#define RANK_FREQ_MED_SESSIONS  8
#define RANK_FREQ_HIGH_SESSIONS 20
#define RANK_HOT_S       60          /*  <60 s -> TIER1                 */
#define RANK_WARM_S      (10*60)     /* <10 min -> old WARM boundary, still used by the TIER2 MAINT clause (see adaptive_clean) */
#define RANK_COLD_S      (60*60)     /*  <1 hr -> TIER2; else TIER3     */

#define RANK1_HOLD_S     3

#define TIER1_MAX_SLOTS   4      /* tier1 tier capacity */

#define TIER1_PIN_MAX     4      /* of TIER1_MAX_SLOTS, how many get a real pin */

#define TIER1_PIN_HOLD_S  RETAIN_FG_AGE_S

#define TIER1_DWELL_S     300    /* min time in tier1 before demotion to tier2 */
#define TIER2_MAX_SLOTS   6
#define TIER2_PIN_MAX     3
#define TIER2_DWELL_S     420    /* min time in tier2 before demotion to tier3 */
#define BG_IDLE_DEBOUNCE_S 30
#define UNTRUSTED_PROMO_DEBOUNCE_S 30
#define CAPACITY_PROMO_DEBOUNCE_S 30

#define BLIP_HEADLESS_MIN_S   600   /* 10 cumulative min of non-genuine ADJ_FOREGROUND */

#define FLAP_WINDOW_MIN_S    (30*60)  /* don't judge a rate until the window is this old, to avoid false alarms from early legitimate re-opens */
#define FLAP_RATE_WARN_THRESH  15.0   /* flaps/hour that triggers a warn */
#define FLAP_WINDOW_RESET_S  (4*60*60) /* rolling window reset period; also re-arms the one-shot warn */

/* File swap */
#define SWAP_FILE "/data/lmk_swap"

/* OOM adj bands */
#define ADJ_FOREGROUND    0
#define ADJ_VISIBLE_MAX   200
#define ADJ_SERVICE_MAX   899
#define ADJ_CACHED_MAX    999
#define ADJ_UNKNOWN       1001

/* Foreground protection window (seconds) */
#define FG_PROTECT_BASE   (30*60)
#define FG_PROTECT_MAX    (6*60*60)
/* Reduced windows under memory pressure */
#define FG_PROTECT_LOW_PCT  40  /* LOW state: % of computed window   */
#define FG_PROTECT_CRIT_S   20  /* CRITICAL state: flat floor (s)    */
#define MIN_BG_KILL_AGE_S    8

/* App retention (oom pinning) */

#define RETAIN_FG_AGE_S  900    /* only pin if used within 15 min    */
#define PIN_SETTLE_S     10
#define LEARN_RESET_GEN     7
#define LEARN_LOG_FILE     "/data/local/lmk_learn.log" /* legacy path, only renamed by score_load() on a gen mismatch; no longer written */
#define RETAIN_MAX_N       6
#define RETAIN_PROTECT_GRACE_S 8
/* T3 critical-RAM sparing: keep a couple of tier1 background apps so light multitasking survives a sweep */
#define T3_SPARE_HOT_N      2

/* Proactive launch trim */
#define LAUNCH_TRIM_AVAIL_PCT     FREE_HIGH_PCT  /* only trim if avail below this   */
#define LAUNCH_TRIM_HEAVY_SWAP_KB (150 * 1024)   /* learned avg_swap_kb -> "heavy"   */
#define LAUNCH_TRIM_TARGET_PCT    (FREE_HIGH_PCT - 3)
#define LAUNCH_TRIM_HEAVY_TARGET_PCT (FREE_HIGH_PCT + 7)
#define LAUNCH_TRIM_WARM_SKIP_S   90  /* skip RAM trim if relaunched within this   */

/* Session tracking */
#define SESSION_GAP_S     60   /* seconds of absence = new session  */

#define SESSION_DURATION_CAP_S     600
#define SESSION_DURATION_BONUS_MAX 150
#define MIN_GENUINE_SESSION_S      20

/* Restart resilience */

#define RESTART_WINDOW_S    240
#define RESTART_CD_BASE_S   30
#define RESTART_CD_SCALE_S  20
#define RESTART_CD_MAX_S   180
#define BOUNCE_KILL_CD_MAX_S (30*60)

/* Kill history / cooldown */
/* 256 slots: one T3 sweep can kill 24 processes at once and a 32-slot ring wrapped within seconds, losing cooldown history */
#define KILL_HIST_SIZE   256

/* Process protection lists (generic AOSP) */
static const char * const NEVER_KILL[] = {
    "zygote","zygote64","system_server","surfaceflinger",
    "android.hardware","audioserver","cameraserver","installd","vold","netd",
    "logd","servicemanager","hwservicemanager","vndservicemanager",
    "lmk_engine","init","ueventd",
    "com.android.systemui",
    "android.process.media",
    "com.android.providers.media",
    "com.android.contacts",
    "com.android.calendar",
    "com.android.deskclock",
    NULL
};

/* Launcher and input-method protection is resolved from the active system
 * defaults at runtime; hard-coded OEM package lists go stale across ROMs. */
#define DYNAMIC_PROTECTED_PKG_MAX 256
static char g_launcher_pkg[DYNAMIC_PROTECTED_PKG_MAX];
static char g_ime_pkg[DYNAMIC_PROTECTED_PKG_MAX];
static char g_media_pkg[DYNAMIC_PROTECTED_PKG_MAX];
static time_t g_dynamic_protected_pkgs_t = 0;

static const char * const SERVICE_EXEMPT[] = {
    "sandboxed_process",               /* webview/chrome sandboxed renderers */
    ":rcs",                            /* carrier RCS messaging service      */
    "googlequicksearchbox:interactor", /* GMS quicksearch interactor         */
    "com.truecaller",                  /* caller-ID/call-screening service;  */

                                        /* respawn cycles, rc 75->85, each    */
                                        /* kill freed 0-30MB RSS. OS re-wakes */
                                        /* it for call events regardless,   */
                                        /* pure battery/relaunch cost, no    */
                                        /* memory benefit.                   */
    NULL
};

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_shutdown_signal = 0;
static bool          g_zram_active  = false;
static char          g_zram_dev[128] = {0};
static char          g_zram_sys[128] = {0};
static long          g_total_ram_kb  = 0;
static time_t        g_last_kill_t   = 0;
static FILE         *g_log           = NULL;

static char          g_true_fg_name[256] = {0};
static char          g_pending_fg_name[256] = {0};
static int           g_pending_fg_streak = 0;
#define FG_DEBOUNCE_TICKS 2 /* candidate must persist this many ticks before accepted */

#define FG_MIN_HOLD_S 3
#define FG_HOLD_WOBBLE_TOLERANCE_S 2

static bool          g_zram_needs_resize = false;

/* Widget providers (dynamic, refreshed every 5 min) */
static char   g_wpkg[64][128];
static int    g_wpkg_count = 0;
static time_t g_wpkg_ts    = 0;

/* Kill history (per-app cooldown + restart detection) */
typedef struct {
    char   name[256];
    time_t killed_at;
    bool   bounced;
    bool   scheduled;
} KillRecord;
static KillRecord g_kill_hist[KILL_HIST_SIZE];
static int        g_kill_hist_idx = 0;

/* Futility tracking */
static int    g_zram_pct_at_start = 0;
static time_t g_zram_kill_start_t = 0;
static time_t g_zram_pause_until  = 0;
#define FUTILITY_WINDOW_S   90
#define FUTILITY_PAUSE_S   120
#define FUTILITY_MIN_DROP    2

/* Rate-limit for zram_pressure_kill() */
static time_t g_zram_kill_last_t  = 0;

/* Rate-limit for oom_pin_retained() to suppress per-tick log spam */
static time_t g_retain_log_t      = 0;

/* Adaptive clean trend tracking */
static int    g_adaptive_prev_zram_pct = 0;
static time_t g_adaptive_trend_t       = 0;

/* Idle mode tracking, updated when true-foreground app changes */
static time_t g_last_fg_change_t = 0;
static time_t g_screen_off_since = 0;  /* 0 = screen currently on */

static int g_zram_used_pct = 0;

static time_t g_rank_cache_last_save = 0;

/* Boot widget-settle: kill paths are suppressed until this timestamp to give
 * AppWidgetService time to write appwidgets.xml after sys.boot_completed. */
static time_t g_widget_settle_until = 0;
#define WIDGET_SETTLE_S  45   /* seconds to wait after boot_completed */

/* Stuck-ZRAM tracking for deep clean */
static time_t g_zram_stuck_since  = 0;   /* when ZRAM first hit STUCK_PCT */
static time_t g_last_deepclean    = 0;   /* timestamp of last deep clean  */

#define RESET_GATE_S       (12*3600)  /* min time since boot/last reset  */
static time_t g_last_full_reset_t = 0;   /* 0 = not done yet this boot   */
static bool   g_reset_in_progress = false; /* tells check_bounced() this kill was scheduled, not organic instability */

#define START_TIME_FILE  "/data/local/tmp/lmk_engine.start"

static time_t g_start_time           = 0;  /* daemon start epoch           */

/* Log rate-limit timestamps (prevent 4+ lines/s when ZRAM saturated) */
static time_t g_log_stuck_t          = 0;  /* last ZRAMstuck cd-active log */
static time_t g_log_crit_t           = 0;  /* last ZRAMcrit log            */
static time_t g_log_zramcd_t         = 0;  /* last ZRAMcd cooldown log     */

static int    g_deepclean_fail_cnt   = 0;  /* consecutive <=2% cleans       */
static time_t g_deepclean_pause_until = 0; /* pause end timestamp          */

#define SCORE_FILE          "/data/local/lmk_scores.dat"

#define SCORE_JOURNAL_FILE          "/data/local/lmk_scores.journal"
#define SCORE_JOURNAL_CONSOLIDATE_N 400  /* consolidate after this many journal-appended entries */

#define CLASSIFY_IDLE_S        240  /* 4 min screen-off before classify starts */
#define SYSCHECK_QUEUE_MAX     128
#define SYSCHECK_DRAIN_PER_TICK  2   /* max dumpsys calls per idle tick   */
#define SYSCHECK_MAX_ATTEMPTS    4   /* give up after this many failed lookups so an unresolvable process alias can't loop forever */
#define CATEGORY_RECONFIRM_S  (3L*24*3600)
#define SCORE_MAX_APPS      260
#define SCORE_MAX           1000   /* final score_compute() clamp only    */

#define FG_COUNT_MAX        100000
#define FG_COUNT_SCALE      100
#define SCORE_SAVE_INTVL    15
#define SCORE_AVG_ALPHA_PCT  25   /* EMA weight for new sample (25%)  */

#define GROWTH_WINDOW_S        600    /* sample window: 10 min             */
#define GROWTH_MIN_BASE_KB     51200  /* ignore <50MB procs (noise)        */
#define GROWTH_STRIKE_PCT      12     /* % growth in a window = 1 strike   */
#define GROWTH_STRIKES_FLAG    6      /* consecutive strikes (~1h) -> flag  */
#define GROWTH_REALERT_S       3600   /* re-log a still-growing flag hourly*/

typedef struct {
    char   name[256];
    int    fg_count;       /* cumulative foreground ticks               */

    time_t raw_last_fg_do_not_read_directly;        /* last foreground timestamp                 */
    int    restart_count;  /* times seen alive <RESTART_WINDOW_S post-kill */
    long   avg_swap_kb;    /* EMA of swap usage in kB                  */

    long   avg_session_duration_s;

    time_t session_start_t;
    int    session_count;  /* distinct foreground sessions (v3)         */
    int    category;       /* 0=native daemon, 1=system app, 2=user app */
    int    dc_kill_count;  /* times killed by deep clean (v5)           */
    int    adaptive_kill_count; /* times killed by adaptive clean (v5)  */
    time_t retain_pin_t;   /* last tick this app was actively pinned; in-memory only */

    time_t last_restart_t;

    int    last_logged_rank;

    int    flap_count;
    time_t flap_window_start;
    bool   flap_warned;
    time_t last_bg;
    bool   was_true_fg;
    bool   dirty;

    time_t last_genuine_fg;

    time_t last_true_fg_seen;

    time_t bg_idle_since;

    time_t rank_fg_hold_start;

    time_t untrusted_promo_since;

    time_t capacity_demote_t;
    int    capacity_demote_from;

    int    static_class_cache;

    long   rss_watch_kb;      /* RSS sampled at start of current window   */
    time_t rss_watch_t;       /* when the current window started         */
    int    rss_growth_strikes;/* consecutive windows with sustained growth*/
    bool   flagged_runaway;   /* confirmed runaway, bias toward killing  */
    time_t runaway_alert_t;   /* last time we logged the alert            */

    bool   category_confirmed;

    time_t category_confirmed_at;

    time_t last_reclaim_t;

    bool   cg_high_set;   /* true if memory.high is currently < max on this app */
    time_t cg_high_set_t; /* when set, for a future safety-net max-duration release */
    bool   cg_high_effective; /* last squeeze freed >= RECLAIM_MIN_FREED_KB; in-memory only, lets an ineffective limit release after SQUEEZE_HIGH_FAILED_RELEASE_S */
    bool   cg_low_set;    /* true if memory.low is currently > 0 on this app */

    time_t fg_hold_start;

    time_t fg_hold_last_genuine_t;

    time_t first_seen;

    time_t adj0_blip_total_s;
    time_t adj0_last_tick;

    int    rank_tier;        /* current dwell-confirmed tier; 0 = not yet seeded */
    time_t tier_entered_t;   /* when rank_tier was last set; 0 = not yet seeded */

    long   rank_computed_seq;

    int    rank_computed_result;

    time_t last_seen_t;
} AppScore;

static AppScore g_scores[SCORE_MAX_APPS];
static int      g_score_count     = 0;

static long g_rank_seq = 0;
static time_t   g_score_last_save = 0;
static bool     g_scores_dirty    = false;
static time_t   g_score_last_compact = 0;
#define SCORE_COMPACT_INTVL_S (24*60*60)  /* run compaction once a day        */
#define SCORE_STALE_PRUNE_S   (30*24*60*60) /* drop entries untouched 30 days */

/* Foreground history - last time each app was in the foreground */
#define FG_HISTORY_SIZE 64
typedef struct { char name[256]; time_t last_seen; } FgHistory;
static FgHistory g_fg_history[FG_HISTORY_SIZE];
static int       g_fg_history_count = 0;

typedef enum {
    PRIO_NEVER,
    PRIO_SEMI_PROTECTED,
    PRIO_BACKGROUND,
    PRIO_CACHED,
    PRIO_JUNK
} Priority;

typedef struct {
    pid_t    pid;
    char     name[256];
    long     oom_adj, rss_kb, swap_kb;
    Priority prio;
    time_t   raw_last_fg_do_not_read_directly;
    int      score;
    int      restart_count;
    long     avg_swap_kb;
    int      bounce_count;
    time_t   bounce_first;      /* first kill in the window               */
    int      rank;
    time_t   last_genuine_fg;

    /* /proc/<pid>/stat field 22 at enumeration; 0 = unreadable, so kill_proc_verified() skips the kill */
    unsigned long long starttime;

    bool     true_fg;
} ProcInfo;

typedef struct {
    long total_kb, free_kb, avail_kb;
    long swap_total_kb, swap_free_kb;
    int  free_pct, avail_pct;
    bool avail_estimated;   /* kernel lacks MemAvailable: derived, see meminfo_parse() */
} MemInfo;

typedef struct {
    char dev[128];
    char sys[128];
    bool active;
    long disksize_kb;
    long used_kb;
    long orig_data_kb;
    long compr_data_kb;
    int  used_pct;
} ZramInfo;

/* Logging */
#define LOG_FILE "/data/local/tmp/lmk_engine.log"
#define LOG_MAX  (2 * 1024 * 1024)

/* Rotation NULLs g_log before fclose() and _log() re-reads it afterwards, so no path sees a dangling FILE*.
 * Single-threaded by design, so no mutex; a failed reopen falls back to stderr and retries every LOG_REOPEN_RETRY_S. */
#define LOG_REOPEN_RETRY_S 30
static time_t g_log_reopen_after = 0;   /* 0 = no reopen pending */

/* Open (or reopen) the log for appending; NULL on failure. */
static FILE *log_open_append(void) {
    return fopen(LOG_FILE, "a");
}

static void log_rotate(void) {
    time_t now = time(NULL);

    if (!g_log) {
        /* Reopen only after a rotation/open failure left no stream; CLI invocations (g_log never set) keep using stderr. */
        if (g_log_reopen_after != 0 && now >= g_log_reopen_after) {
            g_log = log_open_append();
            g_log_reopen_after = g_log ? 0 : now + LOG_REOPEN_RETRY_S;
        }
        return;
    }

    if (fseek(g_log, 0, SEEK_END) != 0) return;   /* size unknown: keep stream */
    long sz = ftell(g_log);
    if (sz < 0 || sz <= (long)LOG_MAX) return;

    /* Rotate. Detach the stream first so g_log can never dangle. */
    FILE *old = g_log;
    g_log = NULL;
    fclose(old);

    char bak[320]; snprintf(bak, sizeof(bak), "%s.old", LOG_FILE);
    bool renamed = (rename(LOG_FILE, bak) == 0);

    /* If rename failed the old data is still in LOG_FILE: truncating ("w") bounds the file at the cost of that history; appending would grow it forever. */
    g_log = fopen(LOG_FILE, renamed ? "a" : "w");
    if (!g_log) g_log = fopen(LOG_FILE, "a");
    if (!g_log) g_log_reopen_after = now + LOG_REOPEN_RETRY_S;
    else        g_log_reopen_after = 0;
}
__attribute__((format(printf, 2, 0)))
static void _log(const char *tag, const char *fmt, va_list ap) {
    log_rotate();                                  /* may replace g_log   */
    FILE *fp = g_log ? g_log : stderr;             /* fetched AFTER rotate */
    time_t now = time(NULL);
    struct tm tmv, *t = localtime_r(&now, &tmv);
    char ts[24];
    if (!t || strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", t) == 0)
        snprintf(ts, sizeof(ts), "00-00 00:00:00");   /* localtime failed */
    fprintf(fp, "[%s][%s] ", ts, tag);
    vfprintf(fp, fmt, ap); fputc('\n', fp); fflush(fp);
}
#define DEFLOG(fn,tag) \
    __attribute__((format(printf, 1, 2))) \
    static void fn(const char *fmt,...){ \
        va_list ap;va_start(ap,fmt);_log(tag,fmt,ap);va_end(ap);}
DEFLOG(logi,"INFO ")
DEFLOG(logw,"WARN ")
DEFLOG(loge,"ERROR")
DEFLOG(logk,"KILL ")

/* Overflow-free three-way compare for qsort(): cmp3(a, b) sorts ascending, cmp3(b, a) descending. */
static inline int cmp3(long long a, long long b) {
    return (a > b) - (a < b);
}

/* Milliseconds from a to b (CLOCK_MONOTONIC samples). tv_sec/tv_nsec are
 * integers, so each difference is taken first and only then converted. */
static double ts_diff_ms(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

/* Meminfo */
/* Returns 0 only for a trustworthy snapshot; on -1 *m is zeroed and must not drive decisions.
 * Without MemAvailable (kernels < 3.14) it is estimated from free + buffers + file lists and flagged in avail_estimated. */
static int meminfo_parse(FILE *f, MemInfo *m) {
    memset(m, 0, sizeof(*m));
    enum { H_TOTAL = 1, H_FREE = 2, H_AVAIL = 4, H_SWT = 8, H_SWF = 16,
           H_BUF = 32, H_FILE_A = 64, H_FILE_I = 128 };
    unsigned seen = 0;
    long buffers = 0, file_a = 0, file_i = 0, v;
    bool negative = false;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        if      (sscanf(line, "MemTotal: %ld kB", &v) == 1)       { m->total_kb = v;      seen |= H_TOTAL; }
        else if (sscanf(line, "MemFree: %ld kB", &v) == 1)        { m->free_kb = v;       seen |= H_FREE; }
        else if (sscanf(line, "MemAvailable: %ld kB", &v) == 1)   { m->avail_kb = v;      seen |= H_AVAIL; }
        else if (sscanf(line, "SwapTotal: %ld kB", &v) == 1)      { m->swap_total_kb = v; seen |= H_SWT; }
        else if (sscanf(line, "SwapFree: %ld kB", &v) == 1)       { m->swap_free_kb = v;  seen |= H_SWF; }
        else if (sscanf(line, "Buffers: %ld kB", &v) == 1)        { buffers = v;          seen |= H_BUF; }
        else if (sscanf(line, "Active(file): %ld kB", &v) == 1)   { file_a = v;           seen |= H_FILE_A; }
        else if (sscanf(line, "Inactive(file): %ld kB", &v) == 1) { file_i = v;           seen |= H_FILE_I; }
        else continue;
        if (v < 0) negative = true;
    }
    bool io_err = ferror(f) != 0;

    if (io_err || negative || !(seen & H_TOTAL) || !(seen & H_FREE) || m->total_kb <= 0)
        goto invalid;

    if (!(seen & H_AVAIL)) {
        const unsigned need = H_BUF | H_FILE_A | H_FILE_I;
        if ((seen & need) != need) goto invalid;
        long est = m->free_kb + buffers + file_a + file_i;
        m->avail_kb = est > m->total_kb ? m->total_kb : est;
        m->avail_estimated = true;
    }
    if (m->free_kb > m->total_kb || m->avail_kb > m->total_kb) goto invalid;

    if (!(seen & H_SWT)) m->swap_total_kb = m->swap_free_kb = 0;
    else if (!(seen & H_SWF) || m->swap_free_kb > m->swap_total_kb)
        m->swap_free_kb = (seen & H_SWF) ? m->swap_total_kb : 0;

    m->free_pct  = (int)(m->free_kb  * 100 / m->total_kb);
    m->avail_pct = (int)(m->avail_kb * 100 / m->total_kb);
    return 0;

invalid:
    memset(m, 0, sizeof(*m));
    return -1;
}

/* Returns 0 with a validated snapshot, -1 otherwise. Callers must fail
 * closed: skip kills, compaction and ZRAM changes for that tick. */
static int read_meminfo(MemInfo *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    int rc = meminfo_parse(f, m);
    fclose(f);
    if (rc != 0) return -1;
    if (m->avail_estimated) {
        static bool warned;
        if (!warned) { warned = true; logw("meminfo: no MemAvailable, using estimate"); }
    }
    if (!g_total_ram_kb) g_total_ram_kb = m->total_kb;
    return 0;
}

static int    g_meminfo_fail_streak;
static time_t g_meminfo_fail_log_t;

/* Rate-limited note for a skipped tick. */
static void meminfo_fail_note(void) {
    g_meminfo_fail_streak++;
    time_t now = time(NULL);
    if (g_meminfo_fail_streak == 1 || now - g_meminfo_fail_log_t >= LOG_RATELIMIT_S) {
        g_meminfo_fail_log_t = now;
        logw("meminfo unreadable or invalid (%d in a row), skipping destructive actions",
             g_meminfo_fail_streak);
    }
}

/* The one definition of CRITICAL RAM: strictly below FREE_CRIT_PCT. RAM_LOW is milder and must not trigger the critical bypasses;
 * avail_pct == FREE_CRIT_PCT is LOW and avail_pct == FREE_LOW_PCT is normal. */
static bool ram_state_critical(const MemInfo *mi) {
    return mi->avail_pct < FREE_CRIT_PCT;
}

/* Process helpers */
static bool name_matches(const char *name, const char * const *list) {
    for (int i = 0; list[i]; i++) if (strstr(name, list[i])) return true;
    return false;
}

static void read_command_value(const char *cmd, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *candidate = strstr(line, "ComponentInfo{");
        if (candidate) candidate += strlen("ComponentInfo{");
        else candidate = line;
        char *slash = strchr(candidate, '/');
        if (!slash) continue;
        char *start = candidate;
        while (start < slash && isspace((unsigned char)*start)) start++;
        size_t n = (size_t)(slash - start);
        while (n && isspace((unsigned char)start[n - 1])) n--;
        if (n == 0 || n >= outsz) continue;
        memcpy(out, start, n);
        out[n] = '\0';
        break;
    }
    pclose(fp);
}

static bool audio_device_running(void) {
    DIR *cards = opendir("/proc/asound");
    if (!cards) return false;
    bool running = false;
    struct dirent *card;
    while (!running && (card = readdir(cards)) != NULL) {
        if (strncmp(card->d_name, "card", 4) != 0) continue;
        /* d_name can be 255 bytes: size every buffer in this chain for the worst case (card 12+255+1, pcm card+1+255+1, status pcm+1+255+7+1) so -Wformat-truncation stays quiet. */
        char card_path[300];
        snprintf(card_path, sizeof(card_path), "/proc/asound/%s", card->d_name);
        DIR *card_dir = opendir(card_path);
        if (!card_dir) continue;
        struct dirent *pcm;
        while (!running && (pcm = readdir(card_dir)) != NULL) {
            if (strncmp(pcm->d_name, "pcm", 3) != 0) continue;
            char pcm_path[600];
            snprintf(pcm_path, sizeof(pcm_path), "%s/%s", card_path, pcm->d_name);
            DIR *pcm_dir = opendir(pcm_path);
            if (!pcm_dir) continue;
            struct dirent *sub;
            while (!running && (sub = readdir(pcm_dir)) != NULL) {
                if (strncmp(sub->d_name, "sub", 3) != 0) continue;
                char status_path[900], status[128];
                snprintf(status_path, sizeof(status_path), "%s/%s/status", pcm_path, sub->d_name);
                FILE *fp = fopen(status_path, "r");
                if (!fp) continue;
                if (fgets(status, sizeof(status), fp) && strstr(status, "RUNNING")) running = true;
                fclose(fp);
            }
            closedir(pcm_dir);
        }
        closedir(card_dir);
    }
    closedir(cards);
    return running;
}

static void refresh_dynamic_protected_pkgs(void) {
    time_t now = time(NULL);
    if (g_dynamic_protected_pkgs_t && now - g_dynamic_protected_pkgs_t < 60) return;
    g_dynamic_protected_pkgs_t = now;
    char old_launcher[DYNAMIC_PROTECTED_PKG_MAX];
    char old_ime[DYNAMIC_PROTECTED_PKG_MAX];
    snprintf(old_launcher, sizeof(old_launcher), "%s", g_launcher_pkg);
    snprintf(old_ime, sizeof(old_ime), "%s", g_ime_pkg);
    /* value is sized like the destination buffers so the copies can't truncate (read_command_value refuses candidates longer than outsz-1). */
    char value[DYNAMIC_PROTECTED_PKG_MAX];
    g_launcher_pkg[0] = '\0';
    read_command_value("cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.HOME 2>/dev/null", value, sizeof(value));
    if (value[0] && !strstr(value, "No activity"))
        snprintf(g_launcher_pkg, sizeof(g_launcher_pkg), "%s", value);
    read_command_value("cmd settings get secure default_input_method 2>/dev/null", value, sizeof(value));
    if (value[0] && strcmp(value, "null") != 0)
        snprintf(g_ime_pkg, sizeof(g_ime_pkg), "%s", value);
    else g_ime_pkg[0] = '\0';
    if (strcmp(old_launcher, g_launcher_pkg) != 0 || strcmp(old_ime, g_ime_pkg) != 0) {
        for (int i = 0; i < g_score_count; i++)
            g_scores[i].static_class_cache = -1;
    }
    g_media_pkg[0] = '\0';
    if (audio_device_running()) {
        FILE *fp = popen("cmd media_session list-sessions 2>/dev/null", "r");
        if (fp) {
            char line[512];
            char session_pkg[DYNAMIC_PROTECTED_PKG_MAX] = {0};
            while (fgets(line, sizeof(line), fp)) {
                char *pkg = strstr(line, "package=");
                if (pkg) {
                    pkg += 8;
                    size_t n = strcspn(pkg, " \t\r\n,}");
                    if (n > 0 && n < sizeof(session_pkg)) {
                        memcpy(session_pkg, pkg, n);
                        session_pkg[n] = '\0';
                    }
                }
                bool playing = strstr(line, "state=3") != NULL ||
                               strstr(line, "state=PlaybackState {state=3") != NULL;
                if (playing && session_pkg[0])
                    snprintf(g_media_pkg, sizeof(g_media_pkg), "%s", session_pkg);
            }
            pclose(fp);
        }
    }
}

static bool package_process_matches(const char *name, const char *component) {
    if (!name || !component || !component[0]) return false;
    const char *slash = strchr(component, '/');
    size_t len = slash ? (size_t)(slash - component) : strlen(component);
    return len > 0 && strncmp(name, component, len) == 0 &&
           (name[len] == '\0' || name[len] == ':');
}
static bool is_launcher_like(const char *n) {
    return package_process_matches(n, g_launcher_pkg);
}
static bool is_ime_like(const char *n) {
    return package_process_matches(n, g_ime_pkg);
}
static bool is_media_player(const char *n) {
    return package_process_matches(n, g_media_pkg);
}

static int classify_score_cat(const char *name);

static AppScore *score_lookup(const char *name);

/* HAL or low-level vendor daemon that must never be kill-targeted. */
static bool is_hal_process(const char *name) {
    if (!name || !name[0]) return false;
    /* Absolute-path native binaries */
    if (name[0] == '/') return true;
    /* HIDL / AIDL HAL service names */
    if (strncmp(name, "android.hardware.", 17) == 0) return true;
    if (strncmp(name, "android.hidl.",     13) == 0) return true;
    if (strncmp(name, "android.frameworks.",19) == 0) return true;
    if (strncmp(name, "android.system.",   15) == 0) return true;
    if (strncmp(name, "vendor.",             7) == 0) return true;
    /* MTK-specific HAL patterns */
    if (strstr(name, "mediatek"))  return true;
    if (strstr(name, ".hal"))      return true;
    if (strstr(name, "ccci"))      return true;
    return false;
}

/* Background service sub-process (colon in the name: a private process spawned by the main one). */
static bool is_bg_service_proc(const char *name) {
    return name && strchr(name, ':') != NULL;
}

/* Exempt from ranking entirely (not ranked or targeted by recents-dismiss). */
static bool is_rank_exempt(const char *name) {
    if (!name || !name[0]) return true;
    if (is_hal_process(name))    return true;
    if (is_bg_service_proc(name)) return true;
    if (name_matches(name, NEVER_KILL)) return true;
    if (is_launcher_like(name))  return true;
    if (is_ime_like(name))       return true;
    /* Native binaries (cat 0) stay hard-exempt. SYSAPP (cat 1) is routed to RANK_BACKGROUND in enumerate_procs() so runaway and idle reclaim still see it;
     * prefer the dumpsys-confirmed category, since the curated prefix list never recognizes these packages. */
    AppScore *as = score_lookup(name);
    int cat = (as && as->category_confirmed) ? as->category : classify_score_cat(name);
    return (cat == 0);
}

/* Defined later; app_rank() needs it for score_dwell_mult_pct(). */
static int score_compute(const AppScore *s);

static int score_dwell_mult_pct(int score) {
    if (score >= 800) return 200;   /* heavy daily-driver: double dwell   */
    if (score >= 500) return 160;
    if (score >= 200) return 130;
    return 100;                     /* baseline/junk: unchanged behavior */
}

static int app_rank(AppScore *as, bool true_fg, time_t recency,
                     bool recency_trusted, int session_count) {
    time_t now = time(NULL);
    int candidate;

    if (recency == 0 && !true_fg) candidate = RANK_TIER3;
    else {
        time_t age = true_fg ? 0 : now - recency;

        if (true_fg || (recency_trusted && age < RANK_HOT_S)) candidate = RANK_TIER1;
        else if (recency_trusted && age < RANK_COLD_S) candidate = RANK_TIER2;
        else                                candidate = RANK_TIER3;

        bool has_genuine_sessions = as && as->avg_session_duration_s >= MIN_GENUINE_SESSION_S;
        if (has_genuine_sessions && session_count >= RANK_FREQ_HIGH_SESSIONS && candidate > RANK_TIER1)
            candidate = RANK_TIER2;
        else if (has_genuine_sessions && session_count >= RANK_FREQ_MED_SESSIONS && candidate > RANK_TIER2)
            candidate = RANK_TIER2;
    }

    if (!as) return candidate;

    if (as->tier_entered_t == 0) {              /* first time seeding this app */
        as->rank_tier = candidate;
        as->tier_entered_t = now;
        return candidate;
    }
    if (candidate < as->rank_tier) {             /* promotion/reopen */

        bool untrusted_tier2_promo = (as->rank_tier == RANK_TIER3) &&
                                      (candidate == RANK_TIER2) &&
                                      !true_fg && !recency_trusted;
        if (untrusted_tier2_promo) {
            if (as->untrusted_promo_since == 0)
                as->untrusted_promo_since = now;
            if ((now - as->untrusted_promo_since) < UNTRUSTED_PROMO_DEBOUNCE_S)
                return as->rank_tier;            /* not held long enough yet */
        } else {
            as->untrusted_promo_since = 0;        /* any other promotion path resets it */
        }

        if (!true_fg && as->capacity_demote_from == candidate &&
            as->capacity_demote_t != 0 &&
            (now - as->capacity_demote_t) < CAPACITY_PROMO_DEBOUNCE_S)
            return as->rank_tier;                /* debounce not satisfied yet */
        as->capacity_demote_t = 0;
        as->capacity_demote_from = 0;
        as->rank_tier = candidate;
        as->tier_entered_t = now;
        as->untrusted_promo_since = 0;   /* debounce satisfied or n/a, clear for next cycle */
        return candidate;
    }
    as->untrusted_promo_since = 0;                /* not promoting this tick */
    if (candidate > as->rank_tier) {             /* demotion: dwell-gated */
        int base_dwell = (as->rank_tier == RANK_TIER1) ? TIER1_DWELL_S :
                          (as->rank_tier == RANK_TIER2) ? TIER2_DWELL_S : 0;

        int dwell = (base_dwell * score_dwell_mult_pct(score_compute(as))) / 100;
        if ((now - as->tier_entered_t) < dwell)
            return as->rank_tier;                /* hold current tier a while longer */
        as->rank_tier = candidate;
        as->tier_entered_t = now;
        return candidate;
    }

    if (true_fg) as->tier_entered_t = now;
    return as->rank_tier;                        /* unchanged */
}

static inline bool is_never_kill_now(const ProcInfo *p) {
    return p->rank == RANK_EXEMPT || p->true_fg;
}

static inline bool is_tier1_background(const ProcInfo *p) {
    return p->rank == RANK_TIER1 && !p->true_fg;
}

static int proc_cmdline(pid_t pid, char *buf, size_t sz) {
    char path[48]; snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    int n = (int)read(fd, buf, sz-1); close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    for (int i = 0; i < n; i++) if (!buf[i]) { buf[i] = '\0'; break; }
    return 0;
}

static long proc_oom_adj(pid_t pid) {
    char path[48]; snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
    int fd = open(path, O_RDONLY); if (fd < 0) return ADJ_UNKNOWN;
    char buf[16];
    int n = (int)read(fd, buf, sizeof(buf)-1); close(fd);
    if (n <= 0) return ADJ_UNKNOWN;
    buf[n] = '\0';
    return atol(buf);
}
static void proc_mem_stats(pid_t pid, long *rss, long *swap) {
    char path[48]; snprintf(path, sizeof(path), "/proc/%d/status", pid);
    *rss = 0; *swap = 0;
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    char buf[4096];
    int n = (int)read(fd, buf, sizeof(buf)-1); close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        long v;
        if      (sscanf(line, "VmRSS: %ld kB", &v)  == 1) *rss  = v;
        else if (sscanf(line, "VmSwap: %ld kB", &v) == 1) *swap = v;
        line = nl ? nl + 1 : NULL;
    }
}

/* ZRAM SYSFS helpers */
static long zram_sysfs_rd(const char *sys, const char *attr) {
    char path[256]; snprintf(path, sizeof(path), "%s/%s", sys, attr);
    FILE *f = fopen(path,"r"); if (!f) return -1;
    long v = -1; fscanf(f,"%ld",&v); fclose(f); return v;
}

/* sysfs writes report success/failure so no configuration flow continues as if a failed write had applied; open() failure, short writes and close() errors surface, logged with path + errno. */
static bool zram_sysfs_wr(const char *sys, const char *attr, const char *val) {
    if (!sys || !attr || !val) return false;
    char path[256]; snprintf(path, sizeof(path), "%s/%s", sys, attr);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        loge("sysfs write open failed: %s: %s", path, strerror(errno));
        return false;
    }
    size_t len = strlen(val);
    ssize_t w = write(fd, val, len);
    int saved = errno;
    int rc = close(fd);
    if (w != (ssize_t)len) {
        loge("sysfs write failed: %s (wrote %zd of %zu bytes): %s",
             path, w < 0 ? (ssize_t)0 : w, len,
             w < 0 ? strerror(saved) : "short write");
        return false;
    }
    if (rc != 0) {
        loge("sysfs write close failed: %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

/* Configure compression before disksize after every reset; algorithm and stream count are best-effort, a disksize failure aborts bring-up. */
static bool zram_configure(long target_bytes) {
    int cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (cpus < 1) cpus = 4;
    char streams[16];
    snprintf(streams, sizeof(streams), "%d\n", cpus);
    if (!zram_sysfs_wr(g_zram_sys, "max_comp_streams", streams))
        logw("ZRAM: max_comp_streams write failed (non-fatal on some kernels)");
    if (!zram_sysfs_wr(g_zram_sys, "comp_algorithm", "lz4\n"))
        logw("ZRAM: comp_algorithm=lz4 rejected — keeping kernel default");
    usleep(50000);
    char disksize[32];
    snprintf(disksize, sizeof(disksize), "%ld\n", target_bytes);
    if (!zram_sysfs_wr(g_zram_sys, "disksize", disksize)) {
        loge("ZRAM: disksize write failed — bring-up aborted");
        return false;
    }
    return true;
}

static long zram_orig_data_kb(long *compr_kb_out) {
    if (!g_zram_sys[0]) return -1;
    char path[256]; snprintf(path, sizeof(path), "%s/mm_stat", g_zram_sys);
    FILE *f = fopen(path,"r"); if (!f) return -1;
    unsigned long long orig = 0, compr = 0;
    int n = fscanf(f, "%llu %llu", &orig, &compr);
    fclose(f);
    if (compr_kb_out) *compr_kb_out = (n >= 2) ? (long)(compr / 1024) : -1;
    return (long)(orig / 1024);
}

static bool zram_find(char *dev, size_t dsz, char *sys, size_t ssz) {
    static const char * const C[] = {"/dev/block/zram0","/dev/zram0",NULL};
    for (int i = 0; C[i]; i++) {
        struct stat st;
        if (stat(C[i], &st) == 0 && S_ISBLK(st.st_mode)) {
            snprintf(dev, dsz, "%s", C[i]);
            const char *b = strrchr(C[i], '/'); b = b ? b+1 : C[i];
            snprintf(sys, ssz, "/sys/block/%s", b);
            return true;
        }
    }
    if (system("modprobe zram 2>/dev/null") == 0) {
        struct stat st;
        for (int w = 0; w < 10; w++) {
            if (stat("/sys/block/zram0", &st) == 0) {
                snprintf(dev, dsz, "/dev/block/zram0");
                snprintf(sys, ssz, "/sys/block/zram0");
                return true;
            }
            usleep(200000);
        }
    }
    return false;
}

/* Returns true only for a well-formed line of this device in /proc/swaps; *used_kb_out gets Used in kB (-1 if absent).
 * Matched by exact filename so the file swap can't contaminate the numbers. */
static bool zram_lookup_proc_swaps(long *used_kb_out) {
    if (used_kb_out) *used_kb_out = -1;
    if (!g_zram_dev[0]) return false;
    FILE *f = fopen("/proc/swaps", "r");
    if (!f) return false;
    bool found = false;
    char line[512];
    bool header = true;
    while (fgets(line, sizeof(line), f)) {
        if (header) { header = false; continue; }   /* skip header line */
        char fn[300], type[32];
        long size_kb = -1, used_kb = -1; int prio = -1;
        int n = sscanf(line, "%299s %31s %ld %ld %d",
                       fn, type, &size_kb, &used_kb, &prio);
        (void)type; (void)size_kb; (void)prio;      /* column anchors only */
        if (n < 4) continue;                        /* incomplete line */
        if (strcmp(fn, g_zram_dev) != 0) continue;  /* other swap device */
        if (used_kb < 0) continue;                  /* malformed Used col */
        if (used_kb_out) *used_kb_out = used_kb;
        found = true;
        break;
    }
    fclose(f);
    return found;
}

/* runtime ZRAM state is derived from /proc/swaps, never assumed: g_zram_active must mirror it after any cycle, resize or recovery. */
static bool zram_is_active_in_proc_swaps(void) {
    return zram_lookup_proc_swaps(NULL);
}

static void zram_read_stats(ZramInfo *z) {
    memset(z, 0, sizeof(*z));
    if (!g_zram_dev[0]) return;
    snprintf(z->dev, sizeof(z->dev), "%s", g_zram_dev);
    snprintf(z->sys, sizeof(z->sys), "%s", g_zram_sys);

    /* ZRAM usage comes only from this device's /proc/swaps line; SwapTotal-SwapFree summed every backend and inflated used_pct.
     * A missing or malformed line reports the device inactive with zeroed usage. */
    long used_kb = -1;
    z->active = zram_lookup_proc_swaps(&used_kb);
    z->used_kb = (z->active && used_kb >= 0) ? used_kb : 0;

    long ds = zram_sysfs_rd(g_zram_sys, "disksize");
    z->disksize_kb = (ds > 0) ? ds / 1024 : 0;

    long orig = zram_orig_data_kb(&z->compr_data_kb);
    if (orig > 0) {
        z->orig_data_kb = orig;
    } else if (z->used_kb > 0) {
        /* mm_stat unavailable: keep the legacy 40% estimate, anchored to the device's real usage rather than the system-wide swap delta. */
        z->orig_data_kb = z->used_kb * 4 / 10;
    }
    /* compr_data_kb stays 0 if mm_stat's second field is unreadable (older kernel format, parse failure); callers must check > 0 and no ratio is invented. */
    if (z->compr_data_kb < 0) z->compr_data_kb = 0;

    if (z->disksize_kb > 0 && z->active)
        z->used_pct = (int)((z->used_kb * 100) / z->disksize_kb);
    else
        z->used_pct = 0;   /* no disksize = unknown, not zero-usage */
}

static pid_t g_kswapd_pid          = -1;
static unsigned long g_last_self_jf    = 0;
static unsigned long g_last_kswapd_jf  = 0;
static time_t        g_last_cpu_smpl_t = 0;

static unsigned long read_proc_cpu_jiffies(pid_t pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    /* comm field can contain spaces/parens, so parse from the last ')' */
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long utime = 0, stime = 0;
    int n = sscanf(p + 2,
        "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
        &utime, &stime);
    if (n != 2) return 0;
    return utime + stime;
}

static unsigned long long read_proc_starttime(pid_t pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long long starttime = 0;
    int n = sscanf(p + 2,
        "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
        &starttime);
    if (n != 1) return 0;
    return starttime;
}

/* Every SIGKILL goes through kill_proc_verified(): the snapshot starttime must match live /proc/<pid>/stat, otherwise the PID was recycled and is not signalled.
 * pidfd_send_signal() closes the remaining check-then-kill race on kernels with pidfd (5.3+). */
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_send_signal
#define __NR_pidfd_send_signal 424
#endif

/* True only when the PID still points at the process captured by enumerate_procs(). */
static bool proc_identity_still_valid(const ProcInfo *p) {
    if (!p || p->pid <= 0) return false;
    if (p->starttime == 0) return false;  /* unverifiable snapshot, refuse */
    unsigned long long cur = read_proc_starttime(p->pid);
    return cur != 0 && cur == p->starttime;
}

/* Verified SIGKILL. The pidfd is opened BEFORE the starttime compare: pidfd_open() pins the process the PID resolves to, so a recycle before it is caught by the compare and one after it is impossible.
 * ENOSYS falls back to kill() after a starttime re-check and keeps a check-then-kill race; other open errors abort so resource pressure never selects the weaker path.
 * Returns 0 delivered, 1 skipped (identity changed, gone or aborted; errno kept), -1 signal failed (errno set). */
static int kill_proc_verified(const ProcInfo *p) {
    if (!p || p->pid <= 0 || p->starttime == 0) {
        if (p) logi("KillSkip: pid=%d [%s] unverifiable snapshot entry",
                    p->pid, p->name);
        return p ? 1 : -1;
    }

    int fd = (int)syscall(__NR_pidfd_open, p->pid, 0);
    if (fd >= 0) {
        /* pidfd pinned, now verify what it is pinned to. */
        if (!proc_identity_still_valid(p)) {
            close(fd);
            logi("KillSkip: pid=%d [%s] identity changed or process gone "
                 "(stale snapshot entry)", p->pid, p->name);
            return 1;
        }
        int rc = (int)syscall(__NR_pidfd_send_signal, fd, SIGKILL, NULL, 0);
        int saved = errno;
        close(fd);
        if (rc == 0) return 0;
        if (saved == ESRCH) return 1;          /* exited meanwhile */
        if (saved != ENOSYS) { errno = saved; return -1; }
        /* ENOSYS from pidfd_send_signal only: fall through to the guarded kill() below. */
    } else {
        int oerr = errno;
        if (oerr == ESRCH) return 1;           /* already gone */
        if (oerr != ENOSYS) {
            /* EMFILE/ENFILE/ENOMEM/EPERM (seccomp)/EINVAL...: do NOT degrade to the racy path; abort this kill. */
            logw("KillAbort: pid=%d [%s] pidfd_open failed (%s) — "
                 "refusing weaker kill() fallback",
                 p->pid, p->name, strerror(oerr));
            errno = oerr;
            return 1;
        }
        /* ENOSYS: kernel has no pidfd_open, genuine "unsupported". */
    }

    /* Fallback: validate as late as possible, then kill(). */
    if (!proc_identity_still_valid(p)) {
        logi("KillSkip: pid=%d [%s] identity changed or process gone "
             "(fallback path)", p->pid, p->name);
        return 1;
    }
    return kill(p->pid, SIGKILL);
}

static pid_t find_kswapd_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    pid_t found = -1;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        /* buffer sized generously: de->d_name is always numeric here, but gcc can't prove a bare %s bounded (the old 64-byte size gave a -Wformat-truncation false positive). */
        char path[300]; snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char comm[64] = {0};
        if (!fgets(comm, sizeof(comm), f)) { fclose(f); continue; }
        fclose(f);
        if (!strncmp(comm, "kswapd", 6)) { found = (pid_t)atoi(de->d_name); break; }
    }
    closedir(d);
    return found;
}

static bool      g_kswapd_pinned     = false;
static cpu_set_t g_little_mask;
static int       g_little_core_count = 0;

#define KSWAPD_EVENT_UNPIN_S   8   /* stay unpinned this long after any kill/squeeze */
static time_t g_kswapd_event_unpin_until = 0;

#define KSWAPD_PIN_DWELL_S     15  /* min time before re-pinning to little cluster */
static time_t g_kswapd_last_transition_t = 0;

/* Called on any kill (record_kill(), any source) or successful squeeze/reclaim, the two events correlated with kswapd spikes; only extends a deadline, no syscalls. */
static void kswapd_note_disruptive_event(void) {
    g_kswapd_event_unpin_until = time(NULL) + KSWAPD_EVENT_UNPIN_S;
}

/* The lowest cpuinfo_max_freq group becomes mask; returns false on a symmetric single-cluster device so the caller skips pinning. */
static bool detect_little_cluster(cpu_set_t *mask, int *count_out) {
    CPU_ZERO(mask);
    *count_out = 0;

    long freqs[64]; int core_ids[64]; int n = 0;
    for (int i = 0; i < 64; i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE *f = fopen(path, "r");
        if (!f) {
            if (i == 0) continue;   /* be tolerant of a missing cpu0 file */
            break;                  /* core IDs are contiguous on Android, first gap = end */
        }
        long freq = 0;
        int got = fscanf(f, "%ld", &freq);
        fclose(f);
        if (got != 1 || freq <= 0) continue;
        freqs[n] = freq; core_ids[n] = i; n++;
    }
    if (n < 2) return false;   /* nothing usable to group */

    long min_freq = freqs[0], max_freq = freqs[0];
    for (int i = 1; i < n; i++) {
        if (freqs[i] < min_freq) min_freq = freqs[i];
        if (freqs[i] > max_freq) max_freq = freqs[i];
    }
    if (min_freq == max_freq) return false;   /* symmetric, no little cluster */

    for (int i = 0; i < n; i++) {
        if (freqs[i] == min_freq) {
            if (core_ids[i] >= 0 && core_ids[i] < CPU_SETSIZE) {
                CPU_SET((size_t)core_ids[i], mask);
                (*count_out)++;
            }
        }
    }
    return *count_out > 0;
}

/* Pins kswapd onto the little cluster. Never retry-loops: on failure (e.g. a PF_NO_SETAFFINITY thread) it keeps kswapd's default affinity, the safe no-op state. */
static void pin_kswapd_to_little_cores(void) {
    if (g_kswapd_pid < 0) g_kswapd_pid = find_kswapd_pid();
    if (g_kswapd_pid <= 0) {
        logw("KswapdPin: kswapd PID not found — skipping");
        return;
    }
    if (!detect_little_cluster(&g_little_mask, &g_little_core_count)) {
        logi("KswapdPin: single-cluster/symmetric CPU topology — skipping "
             "(no little cluster to isolate kswapd onto)");
        return;
    }
    if (sched_setaffinity(g_kswapd_pid, sizeof(cpu_set_t), &g_little_mask) != 0) {
        logw("KswapdPin: sched_setaffinity failed: %s — kswapd left at "
             "default affinity", strerror(errno));
        return;
    }
    g_kswapd_pinned = true;
    logi("KswapdPin: kswapd (pid=%d) pinned to %d little core(s)",
         g_kswapd_pid, g_little_core_count);
}

/* Unpins kswapd; called only at ZRAM_CRIT_PCT, where reclaim should be as fast as possible and restricting it to the little cluster could worsen a near-OOM. */
static void unpin_kswapd(void) {
    if (!g_kswapd_pinned || g_kswapd_pid <= 0) return;
    cpu_set_t all_mask;
    CPU_ZERO(&all_mask);
    long ncpus = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus < 1) ncpus = 8;
    for (int i = 0; i < ncpus && i < CPU_SETSIZE; i++) CPU_SET((size_t)i, &all_mask);
    if (sched_setaffinity(g_kswapd_pid, sizeof(cpu_set_t), &all_mask) == 0) {
        g_kswapd_pinned = false;
        logi("KswapdPin: ZRAM_CRIT_PCT reached — unpinned kswapd to all cores");
    } else {
        logw("KswapdPin: unpin failed: %s", strerror(errno));
    }
}

/* Re-pins to the little cluster once pressure drops under STUCK after a CRIT unpin; no-op if already pinned or no little cluster was found. */
static void repin_kswapd_if_needed(void) {
    if (g_kswapd_pinned || g_little_core_count == 0) return;
    if (g_kswapd_pid <= 0) return;
    if (sched_setaffinity(g_kswapd_pid, sizeof(cpu_set_t), &g_little_mask) == 0) {
        g_kswapd_pinned = true;
        logi("KswapdPin: pressure eased below STUCK — re-pinned to little cores");
    } else {
        logw("KswapdPin: re-pin failed: %s", strerror(errno));
    }
}

/* Returns self/kswapd CPU usage as % of one core since the previous
 * call. First call always returns 0/0 (no prior sample to diff against). */
static void cpu_track_sample(double *self_pct_out, double *kswapd_pct_out) {
    *self_pct_out = 0.0; *kswapd_pct_out = 0.0;
    if (g_kswapd_pid < 0) g_kswapd_pid = find_kswapd_pid();

    unsigned long self_jf   = read_proc_cpu_jiffies(getpid());
    unsigned long kswapd_jf = (g_kswapd_pid > 0) ? read_proc_cpu_jiffies(g_kswapd_pid) : 0;
    time_t now = time(NULL);
    long clk_tck = sysconf(_SC_CLK_TCK);

    if (g_last_cpu_smpl_t > 0 && now > g_last_cpu_smpl_t && clk_tck > 0) {
        double elapsed = (double)(now - g_last_cpu_smpl_t);
        double tck = (double)clk_tck;
        /* A counter that went backwards (failed read returns 0, pid changed) must give 0%, not an unsigned wrap-around. */
        unsigned long self_d   = self_jf   >= g_last_self_jf   ? self_jf   - g_last_self_jf   : 0;
        unsigned long kswapd_d = kswapd_jf >= g_last_kswapd_jf ? kswapd_jf - g_last_kswapd_jf : 0;
        *self_pct_out = 100.0 * (double)self_d / tck / elapsed;
        if (g_kswapd_pid > 0)
            *kswapd_pct_out = 100.0 * (double)kswapd_d / tck / elapsed;
    }
    g_last_self_jf    = self_jf;
    g_last_kswapd_jf  = kswapd_jf;
    g_last_cpu_smpl_t = now;
}

/* Shared bring-up: reset, configure, mkswap, swapon, then verify in /proc/swaps; never resets a device already swapped on. */
static bool zram_bring_up(long target_bytes, const char *who) {
    if (zram_is_active_in_proc_swaps()) {
        logi("%s: device already swapped on — skipping reset", who);
        return true;
    }
    if (!zram_sysfs_wr(g_zram_sys, "reset", "1\n")) {
        loge("%s: reset stage failed", who);
        return false;
    }
    usleep(100000);
    if (!zram_configure(target_bytes)) {
        loge("%s: configure stage failed after reset", who);
        return false;
    }
    usleep(100000);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "mkswap %s >/dev/null 2>&1", g_zram_dev);
    if (system(cmd) != 0) {
        loge("%s: mkswap stage failed", who);
        return false;
    }
    if (swapon(g_zram_dev, 0) < 0) {
        loge("%s: swapon stage failed: %s", who, strerror(errno));
        return false;
    }
    if (!zram_is_active_in_proc_swaps()) {
        loge("%s: swapon returned success but device missing from /proc/swaps",
             who);
        return false;
    }
    return true;
}

#define ZRAM_CYCLE_RECOVER_ATTEMPTS 3

/* Bounded recovery after a failed bring-up: re-attempt the full sequence with backoff rather than leave the device unswapped or wrongly marked active. */
static bool zram_bring_up_recover(long target_bytes, const char *who) {
    for (int r = 1; r <= ZRAM_CYCLE_RECOVER_ATTEMPTS; r++) {
        logw("%s: recovery attempt %d/%d", who, r, ZRAM_CYCLE_RECOVER_ATTEMPTS);
        usleep(1000000);
        if (zram_bring_up(target_bytes, who)) return true;
    }
    return false;
}

/* The one bring-up policy: one attempt, bounded recovery (ZRAM_CYCLE_RECOVER_ATTEMPTS, 1 s backoff), then g_zram_active is re-derived from /proc/swaps. */
static bool zram_bring_up_synced(long target_bytes, const char *who) {
    bool ok = zram_bring_up(target_bytes, who);
    if (!ok) ok = zram_bring_up_recover(target_bytes, who);
    g_zram_active = zram_is_active_in_proc_swaps();
    if (!g_zram_active)
        loge("%s: bring-up + recovery failed — ZRAM inactive "
             "(g_zram_active=false, verified via /proc/swaps)", who);
    else if (!ok)
        logw("%s: bring-up reported failure but device is in /proc/swaps — "
             "state synced to active", who);
    return g_zram_active;
}

static void zram_setup(void) {
    if (!g_zram_dev[0] || !g_zram_sys[0]) return;
    if (g_total_ram_kb <= 0) {
        MemInfo mi;
        if (read_meminfo(&mi) != 0 || mi.total_kb <= 0) {
            loge("ZRAM: cannot determine RAM size"); return;
        }
        g_total_ram_kb = mi.total_kb;
    }

    long target_bytes = (long)g_total_ram_kb * 1024LL * ZRAM_SIZE_PCT / 100;
    logi("ZRAM: target %ldMB (%d%% of %ldMB RAM)",
         target_bytes/1024/1024, ZRAM_SIZE_PCT, g_total_ram_kb/1024);

    bool already_active = zram_is_active_in_proc_swaps();

    long current_bytes = 0;
    long ds = zram_sysfs_rd(g_zram_sys, "disksize");
    if (ds > 0) current_bytes = ds;

    bool within_range = (current_bytes > 0) &&
                        (labs(current_bytes - target_bytes) <= target_bytes / 50);

    if (already_active && within_range) {
        g_zram_active = true;
        logi("ZRAM: already active at %ldMB (within range) — adopted",
             current_bytes/1024/1024);
        return;
    }

    if (already_active && !within_range) {
        logi("ZRAM: active but size %ldMB != target %ldMB — resizing",
             current_bytes/1024/1024, target_bytes/1024/1024);
        ZramInfo z; zram_read_stats(&z);
        if (z.used_pct > 70) {
            logw("ZRAM: too full (%d%%) to safely resize — adopting as-is; "
                 "will resize automatically when ZRAM drops to ≤50%%",
                 z.used_pct);
            g_zram_active       = true;
            g_zram_needs_resize = true;
            return;
        }
        bool swapoff_ok = false;
        for (int t = 0; t < 3 && !swapoff_ok; t++) {
            if (swapoff(g_zram_dev) == 0) { swapoff_ok = true; break; }
            if (t < 2) usleep(200000); /* transient EBUSY at boot, brief backoff */
        }
        if (!swapoff_ok) {

            loge("ZRAM: swapoff failed after 3 attempts: %s — adopting as-is; "
                 "will retry resize once ZRAM drops to <=50%% used",
                 strerror(errno));
            g_zram_active       = zram_is_active_in_proc_swaps();
            g_zram_needs_resize = g_zram_active;
            return;
        }
        /* Device is verifiably off from here until bring-up says otherwise. */
        g_zram_active = false;
        usleep(150000);
        already_active = false;
    }

    if (!already_active) {
        /* Same retry/recovery policy as cycle and deferred resize; g_zram_active is synced from /proc/swaps inside. */
        if (!zram_bring_up_synced(target_bytes, "ZRAM")) return;
        logi("ZRAM: active on %s (%ldMB)", g_zram_dev, target_bytes/1024/1024);
    }
}

/* Structured outcome so callers (daemon loop, --force-zram-cycle CLI) can tell why nothing happened or what failed; /proc/swaps remains the final truth. */
typedef enum {
    ZC_OK = 0,            /* cycled and verified active in /proc/swaps   */
    ZC_NOT_ACTIVE,        /* no device, or not swapped on (nothing to do) */
    ZC_GATE_SKIPPED,      /* avail_kb safety gate refused (not an error)  */
    ZC_SWAPOFF_FAILED,    /* could not swapoff; device left as it was     */
    ZC_BRINGUP_FAILED,    /* swapoff done, bring-up + recovery failed     */
    ZC_MEMINFO_FAILED     /* no valid memory snapshot: refused, fail closed */
} ZramCycleResult;

static const char *zram_cycle_result_str(ZramCycleResult r) {
    switch (r) {
    case ZC_OK:             return "ok";
    case ZC_NOT_ACTIVE:     return "device not active as swap";
    case ZC_GATE_SKIPPED:   return "refused by avail_kb safety gate";
    case ZC_SWAPOFF_FAILED: return "swapoff failed";
    case ZC_BRINGUP_FAILED: return "bring-up/recovery failed";
    case ZC_MEMINFO_FAILED: return "memory snapshot invalid";
    }
    return "unknown";
}

/* Full cycle: swapoff, then bring-up with bounded recovery; g_zram_active is re-derived from /proc/swaps after every stage. */
static ZramCycleResult zram_cycle(bool bypass_gate) {
    if (!g_zram_dev[0]) return ZC_NOT_ACTIVE;
    /* Never trust the cached flag: derive it from /proc/swaps. */
    g_zram_active = zram_is_active_in_proc_swaps();
    if (!g_zram_active) return ZC_NOT_ACTIVE;
    MemInfo mi;
    if (read_meminfo(&mi) != 0) {
        loge("ZRAMcycle: memory snapshot invalid, refusing to swapoff (bypass does not apply)");
        return ZC_MEMINFO_FAILED;
    }
    ZramInfo z; zram_read_stats(&z);

    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
    if (mi.avail_kb < need_kb && !bypass_gate) {
        logi("ZRAMcycle: need %ldMB free (orig=%ldMB + margin), have %ldMB — skip",
             need_kb/1024, z.orig_data_kb/1024, mi.avail_kb/1024);
        return ZC_GATE_SKIPPED;
    }
    if (mi.avail_kb < need_kb && bypass_gate) {
        logw("ZRAMcycle: avail_kb gate would have skipped (need %ldMB, have "
             "%ldMB) — proceeding anyway, --force-zram-cycle --yes-i-know",
             need_kb/1024, mi.avail_kb/1024);
    }

    logi("ZRAMcycle: flushing (used=%ldMB orig=%ldMB avail=%ldMB)…",
         z.used_kb/1024, z.orig_data_kb/1024, mi.avail_kb/1024);

    int zc_attempt, zc_rc = -1;
    for (zc_attempt = 1; zc_attempt <= ZRAM_CYCLE_RETRIES; zc_attempt++) {
        zc_rc = swapoff(g_zram_dev);
        if (zc_rc == 0) break;
        if (errno != EINTR) break;
        usleep(ZRAM_CYCLE_RETRY_BACKOFF_US);
    }
    if (zc_rc < 0) {
        loge("ZRAMcycle: swapoff failed after %d attempt(s): %s — abandoning "
             "this cycle, will retry next interval", zc_attempt, strerror(errno));
        g_zram_active = zram_is_active_in_proc_swaps();   /* kernel truth */
        return ZC_SWAPOFF_FAILED;
    }
    if (zc_attempt > 1) {
        logi("ZRAMcycle: swapoff succeeded on attempt %d/%d",
             zc_attempt, ZRAM_CYCLE_RETRIES);
    }

    /* From here the device is swapped OFF; runtime status must say so until a verified bring-up says otherwise. */
    g_zram_active = false;
    usleep(100000);

    long target_bytes = (long)g_total_ram_kb * 1024LL * ZRAM_SIZE_PCT / 100;

    /* The device was verifiably off, so a fresh reset per attempt is safe; the helper syncs g_zram_active. */
    if (zram_bring_up_synced(target_bytes, "ZRAMcycle")) {
        logi("ZRAMcycle: done, ZRAM fresh (%ldMB)", target_bytes/1024/1024);
        g_zram_pct_at_start = 0;
        g_zram_kill_start_t = 0;
        g_zram_pause_until  = 0;
        return ZC_OK;       /* zram_bring_up_synced() already verified /proc/swaps */
    }
    loge("ZRAMcycle: ZRAM features stay disabled until the next successful "
         "setup (daemon restart or manual --force-zram-cycle)");
    return ZC_BRINGUP_FAILED;
}

#define ZRAM_RESIZE_STUCK_WARN_S 1800  /* 30min */
static time_t g_zram_resize_deferred_since = 0;
static time_t g_zram_resize_defer_log_t = 0;
static bool   g_zram_resize_stuck_warned = false;
static void zram_try_deferred_resize(MemInfo *mi) {
    if (!g_zram_active || !g_zram_dev[0] || !g_zram_sys[0]) return;

    ZramInfo z; zram_read_stats(&z);
    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
    if (z.used_pct > 50 || mi->avail_kb < need_kb) {
        time_t now = time(NULL);
        if (g_zram_resize_deferred_since == 0)
            g_zram_resize_deferred_since = now;
        long stuck_s = now - g_zram_resize_deferred_since;
        if (now - g_zram_resize_defer_log_t >= LOG_RATELIMIT_S) {
            g_zram_resize_defer_log_t = now;
            logi("ZRAMresize: deferred %lds (used=%d%% need<=50%%, "
                 "avail=%ldMB need=%ldMB)",
                 stuck_s, z.used_pct, mi->avail_kb/1024, need_kb/1024);
        }
        if (stuck_s >= ZRAM_RESIZE_STUCK_WARN_S && !g_zram_resize_stuck_warned) {
            g_zram_resize_stuck_warned = true; /* once per stuck episode */
            loge("ZRAMresize: stuck deferred for %lds — resize target not "
                 "yet applied", stuck_s);
        }
        return;
    }
    g_zram_resize_deferred_since = 0; /* conditions cleared - reset tracker */
    g_zram_resize_stuck_warned = false;

    long target_bytes = (long)g_total_ram_kb * 1024LL * ZRAM_SIZE_PCT / 100;
    logi("ZRAMresize: conditions met (ZRAM=%d%% avail=%ldMB orig=%ldMB) — "
         "resizing %ldMB→%ldMB",
         z.used_pct, mi->avail_kb/1024, z.orig_data_kb/1024,
         z.disksize_kb/1024, target_bytes/1024/1024);

    if (swapoff(g_zram_dev) < 0) {
        loge("ZRAMresize: swapoff failed: %s — will retry", strerror(errno));
        return;
    }
    usleep(150000);

    /* same staged bring-up + bounded recovery as the cycle path; g_zram_active tracks the kernel truth at every exit */
    g_zram_active = false;
    bool up = zram_bring_up_synced(target_bytes, "ZRAMresize");
    g_zram_needs_resize = false;  /* meaningless while inactive; next boot's zram_setup() owns the fresh setup */
    if (up) {
        g_zram_pct_at_start = 0;
        g_zram_kill_start_t = 0;
        g_zram_pause_until  = 0;
        logi("ZRAMresize: complete — ZRAM now %ldMB (%d%% of RAM)",
             target_bytes/1024/1024, ZRAM_SIZE_PCT);
    }
}

/* PSI helpers: /proc/pressure/memory gives the real kernel pressure signal. */
static bool g_psi_available = false;

static void psi_check_available(void) {
    FILE *f = fopen("/proc/pressure/memory", "r");
    if (f) { g_psi_available = true; fclose(f); }
    else    { g_psi_available = false;
              logw("PSI: /proc/pressure/memory unavailable"); }
}

/* Returns memory "some" avg10 PSI stall %, or -1.0 on error. */
static double psi_mem_avg10(void) {
    if (!g_psi_available) return -1.0;
    FILE *f = fopen("/proc/pressure/memory", "r");
    if (!f) return -1.0;
    char line[128]; double avg10 = -1.0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "some", 4) == 0) {
            sscanf(line, "some avg10=%lf", &avg10); break;
        }
    }
    fclose(f); return avg10;
}

#define PSI_MEM_ESCALATE_PCT  8.0   /* avg10 > 8%  -> bump adaptive tier +1 */
#define PSI_MEM_URGENT_PCT   25.0   /* avg10 > 25% -> bump adaptive tier +2  */

/* PSI event-driven monitor */

static int g_psi_fd = -1;

static void psi_monitor_init(void) {
    if (!g_psi_available) return;
    int fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        logw("PSI: monitor open failed (%s) — falling back to tick-only PSI reads",
             strerror(errno));
        return;
    }
    const char trig[] = "full 100000 1000000";
    if (write(fd, trig, sizeof(trig)) < 0) {
        logw("PSI: monitor trigger register failed (%s) — falling back to "
             "tick-only PSI reads", strerror(errno));
        close(fd);
        return;
    }

    const char trig_some[] = "some 70000 1000000";
    if (write(fd, trig_some, sizeof(trig_some)) < 0) {
        logw("PSI: 'some' trigger register failed (%s) — 'full' trigger "
             "still armed", strerror(errno));
    }
    g_psi_fd = fd;
    logi("PSI: event monitor armed (full-stall >100ms/1s + partial-stall "
         ">70ms/1s window, speculative threshold)");
}

/* Sleeps up to sleep_us but wakes at once if the PSI trigger fires; plain usleep() when the monitor isn't armed (g_psi_fd < 0). */
static bool wait_for_next_tick(useconds_t sleep_us) {
    if (g_psi_fd < 0) { usleep(sleep_us); return false; }
    struct pollfd pfd = { .fd = g_psi_fd, .events = POLLPRI };
    int timeout_ms = (int)(sleep_us / 1000);
    if (timeout_ms < 1) timeout_ms = 1;
    int r = poll(&pfd, 1, timeout_ms);
    if (r > 0 && (pfd.revents & POLLPRI)) {
        static time_t last_log = 0;
        time_t now = time(NULL);
        if (now - last_log >= LOG_RATELIMIT_S) {
            logi("PSI: pressure trigger fired — reacting early");
            last_log = now;
        }
        return true;
    }
    return false;
}

/* Dynamic swappiness: higher ZRAM usage lowers swappiness to preserve headroom. */
static void update_swappiness(int zram_pct) {

#define SWAP_HYST 3
    static int tier = 0; /* 0=100 1=80 2=60 3=40 4=20 */

    static const int enter[]   = {  0, 60, 82, 86, 94 }; /* rising boundary into tier i */
    static const int target[]  = {100, 80, 60, 40, 20 };

    int t = tier;
    while (t < 4 && zram_pct >= enter[t + 1] + SWAP_HYST) t++;
    while (t > 0 && zram_pct <  enter[t]     - SWAP_HYST) t--;
    if (t == tier) return;
    tier = t;

    FILE *f = fopen("/proc/sys/vm/swappiness", "w");
    if (f) { fprintf(f, "%d\n", target[tier]); fclose(f); }
    logi("Swappiness: ZRAM=%d%% → %d (tier=%d)", zram_pct, target[tier], tier);
}

/* Sleep / doze detection: from reference engine */

#define SCREEN_POLL_INTVL_S 5

/* sysfs backlight state is resolved once, then read with plain open()/read(): zero fork(), unlike the old popen("cmd power ...") (12 fork()+sh -c per minute while asleep). */
static char g_backlight_path[192] = {0};
static bool g_backlight_resolved  = false;

/* Stores the brightness when a readable sysfs file exists; false if the device exposes none (caller falls back to `cmd power`). A file that vanishes invalidates the cache so the next poll re-scans. */
static bool backlight_read(long *out_val) {
    if (!g_backlight_resolved) {
        g_backlight_resolved = true;
        g_backlight_path[0] = '\0';
        if (access("/sys/class/leds/lcd-backlight/brightness", R_OK) == 0) {
            snprintf(g_backlight_path, sizeof(g_backlight_path),
                     "/sys/class/leds/lcd-backlight/brightness");
        } else {
            DIR *d = opendir("/sys/class/backlight");
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    /* %.64s bounds the dirent name so the composed path provably fits g_backlight_path. */
                    snprintf(g_backlight_path, sizeof(g_backlight_path),
                             "/sys/class/backlight/%.64s/brightness",
                             de->d_name);
                    if (access(g_backlight_path, R_OK) == 0)
                        break;
                    g_backlight_path[0] = '\0';
                }
                closedir(d);
            }
        }
    }
    if (!g_backlight_path[0]) return false;

    int fd = open(g_backlight_path, O_RDONLY);
    if (fd < 0) { g_backlight_resolved = false; return false; }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) { g_backlight_resolved = false; return false; }
    buf[n] = '\0';
    *out_val = atol(buf);
    return true;
}

/* Some panels keep their last brightness while blanked; bl_power == 4 (FB_BLANK_POWERDOWN) means off. Only /sys/class/backlight exposes it. */
static bool backlight_blanked(void) {
    if (!g_backlight_path[0]) return false;
    if (!strstr(g_backlight_path, "/sys/class/backlight/")) return false;
    char ppath[192];
    snprintf(ppath, sizeof(ppath), "%s", g_backlight_path);
    char *slash = strrchr(ppath, '/');
    if (!slash) return false;
    snprintf(slash + 1, sizeof(ppath) - (size_t)(slash + 1 - ppath), "bl_power");
    int fd = open(ppath, O_RDONLY);
    if (fd < 0) return false;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return atoi(buf) == 4;   /* FB_BLANK_POWERDOWN */
}

static bool is_screen_off(void) {
    static time_t last_check = 0;
    static bool   cached = false;
    time_t now = time(NULL);
    if (now - last_check < SCREEN_POLL_INTVL_S) return cached;
    last_check = now;

    /* Fast path: sysfs backlight brightness == 0 -> panel off. No fork. */
    long bl = -1;
    if (backlight_read(&bl)) {
        cached = (bl <= 0) || backlight_blanked();
        return cached;
    }

    /* Fallback for devices without a readable sysfs backlight: cmd power get-wakefulness (Android 11+), else dumpsys power | grep, still far lighter than a full dumpsys. */
    FILE *p = popen("cmd power get-wakefulness 2>/dev/null || "
                    "dumpsys power 2>/dev/null | grep -o 'mWakefulness=[A-Za-z]*'",
                    "r");
    if (!p) return cached;
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), p)) { pclose(p); return cached; }
    pclose(p);

    cached = strstr(buf, "Asleep") != NULL ||
             strstr(buf, "Dozing") != NULL ||
             strstr(buf, "asleep") != NULL ||
             strstr(buf, "dozing") != NULL;
    return cached;
}

#define DOZE_POLL_INTVL_S 30   /* deep-doze transitions are minute-scale; polling every 5 s was pure fork() churn */
static bool is_doze_active(void) {
    static time_t last_check = 0;
    static bool   cached = false;
    time_t now = time(NULL);
    if (now - last_check < DOZE_POLL_INTVL_S) return cached;
    /* Deep doze only happens with the screen off and is_screen_off() no longer forks, so skip the cmd deviceidle popen while the screen is on. */
    if (!is_screen_off()) {
        last_check = now;
        cached = false;
        return cached;
    }
    last_check = now;
    /* cmd deviceidle is the official sub-command; one line of output (IDLE/ACTIVE). */
    FILE *p = popen("cmd deviceidle get deep 2>/dev/null", "r");
    if (!p) return cached;
    char buf[32] = {0};
    fgets(buf, sizeof(buf), p); pclose(p);
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n-1]=='\n' || buf[n-1]==' ' || buf[n-1]=='\r')) buf[--n] = '\0';
    cached = strcmp(buf, "IDLE") == 0;
    return cached;
}

/* LMKD minfree cleanup: stops stock lmkd from overriding our kill thresholds. */
static void lmkd_minfree_cleanup(void) {
    if (access("/system/bin/resetprop", X_OK) != 0 &&
        access("/data/adb/magisk/resetprop", X_OK) != 0) return;
    system("resetprop -d sys.lmk.minfree_levels 2>/dev/null; "
           "resetprop lmkd.reinit 1 2>/dev/null");
    logi("LMKD: cleared minfree_levels, reinit sent");
}

/* File swap */
#define SWAP_MIN_MB 1
#define SWAP_MAX_MB 16384   /* 16 GiB: above this a flash-backed swap file is not sane */

/* Strict decimal parse of the --swap argument. */
static bool parse_swap_mb(const char *arg, int *out) {
    if (!arg || !isdigit((unsigned char)arg[0])) return false;
    char *end = NULL;
    errno = 0;
    long v = strtol(arg, &end, 10);
    if (errno != 0 || *end != '\0' || v < SWAP_MIN_MB || v > SWAP_MAX_MB) return false;
    *out = (int)v;
    return true;
}

/* 1 = path is listed in /proc/swaps (exact match on the first column),
 * 0 = not listed, -1 = /proc/swaps unreadable (caller must not guess). */
static int swap_path_active(const char *path) {
    FILE *f = fopen("/proc/swaps", "r");
    if (!f) return -1;
    char line[512];
    int found = 0;
    bool header = true;
    while (fgets(line, sizeof(line), f)) {
        if (header) { header = false; continue; }
        char name[300];
        if (sscanf(line, "%299s", name) == 1 && !strcmp(name, path)) { found = 1; break; }
    }
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : found;
}

/* swapoff must succeed and be confirmed before the file is touched. */
static bool swap_off_verified(const char *path) {
    if (swapoff(path) < 0) {
        fprintf(stderr, "swapoff %s failed: %s. File left untouched.\n", path, strerror(errno));
        return false;
    }
    if (swap_path_active(path) != 0) {
        fprintf(stderr, "%s is still listed in /proc/swaps after swapoff. File left untouched.\n", path);
        return false;
    }
    return true;
}

/* Write mb MiB of zeros to a new 0600 file. Removes the partial file on failure. */
static bool swap_file_fill(const char *path, int mb) {
    struct statvfs vfs;
    if (statvfs("/data", &vfs) == 0 &&
        (unsigned long long)vfs.f_bavail * vfs.f_frsize < (unsigned long long)mb * 1048576ULL) {
        fprintf(stderr, "Not enough free space on /data for %dMB\n", mb);
        return false;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { fprintf(stderr, "create %s: %s\n", path, strerror(errno)); return false; }
    char *zeros = calloc(1, 1048576);
    bool ok = zeros != NULL;
    if (ok && fchmod(fd, 0600) != 0) { fprintf(stderr, "chmod %s: %s\n", path, strerror(errno)); ok = false; }
    for (int i = 0; ok && i < mb; i++) {
        size_t left = 1048576;
        const char *q = zeros;
        while (left > 0) {
            ssize_t w = write(fd, q, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "write %s: %s\n", path, strerror(errno));
                ok = false;
                break;
            }
            q += w; left -= (size_t)w;
        }
    }
    free(zeros);
    if (ok && fsync(fd) != 0) { fprintf(stderr, "fsync %s: %s\n", path, strerror(errno)); ok = false; }
    if (close(fd) != 0 && ok) { fprintf(stderr, "close %s: %s\n", path, strerror(errno)); ok = false; }
    if (!ok) unlink(path);
    return ok;
}

/* Returns 0 on success, 1 on any failure. */
static int swap_create(int mb) {
    bool fresh = false;
    struct stat st;
    if (stat(SWAP_FILE, &st) == 0) {
        long cur_mb = (long)(st.st_size / 1048576);
        int act = swap_path_active(SWAP_FILE);
        if (act < 0) { fprintf(stderr, "Cannot read /proc/swaps, aborting\n"); return 1; }
        if (cur_mb == mb) {
            if (act) { printf("Swap already active: %s (%dMB)\n", SWAP_FILE, mb); return 0; }
        } else {
            if (act && !swap_off_verified(SWAP_FILE)) return 1;
            if (unlink(SWAP_FILE) != 0 && errno != ENOENT) {
                fprintf(stderr, "unlink %s: %s\n", SWAP_FILE, strerror(errno));
                return 1;
            }
            logi("Swap: removed old file (%ldMB)", cur_mb);
            fresh = true;
        }
    } else if (errno == ENOENT) {
        fresh = true;
    } else {
        fprintf(stderr, "stat %s: %s\n", SWAP_FILE, strerror(errno));
        return 1;
    }

    if (fresh) {
        printf("Creating swap file %s (%dMB)...\n", SWAP_FILE, mb);
        if (!swap_file_fill(SWAP_FILE, mb)) return 1;
        if (system("mkswap " SWAP_FILE " >/dev/null 2>&1") != 0) {
            fprintf(stderr, "mkswap failed, removing %s\n", SWAP_FILE);
            unlink(SWAP_FILE);
            return 1;
        }
    }
    if (swapon(SWAP_FILE, 0) < 0) {
        fprintf(stderr, "swapon %s failed: %s%s\n", SWAP_FILE, strerror(errno),
                fresh ? " (new file removed)" : "");
        if (fresh) unlink(SWAP_FILE);
        return 1;
    }
    if (swap_path_active(SWAP_FILE) != 1) {
        fprintf(stderr, "swapon succeeded but %s is not in /proc/swaps\n", SWAP_FILE);
        return 1;
    }
    printf("Swap active: %s (%dMB)\n", SWAP_FILE, mb);
    return 0;
}

/* Returns 0 on success (or nothing to delete), 1 on failure. */
static int swap_delete(void) {
    struct stat st;
    if (stat(SWAP_FILE, &st) != 0) {
        if (errno == ENOENT) { printf("No swap file at %s\n", SWAP_FILE); return 0; }
        fprintf(stderr, "stat %s: %s\n", SWAP_FILE, strerror(errno));
        return 1;
    }
    int act = swap_path_active(SWAP_FILE);
    if (act < 0) { fprintf(stderr, "Cannot read /proc/swaps, aborting\n"); return 1; }
    if (act && !swap_off_verified(SWAP_FILE)) return 1;
    if (unlink(SWAP_FILE) != 0) { fprintf(stderr, "unlink %s: %s\n", SWAP_FILE, strerror(errno)); return 1; }
    printf("Swap removed: %s\n", SWAP_FILE);
    return 0;
}

/* Active widget providers */
#define APPWIDGET_XML     "/data/system/appwidgets.xml"
#define APPWIDGET_XML_U0  "/data/system/users/0/appwidgets.xml"
#define WIDGET_RELOAD_S    300
#define WIDGET_RETRY_S      15   /* short retry until first successful load   */

/* Parse widget package names from an open XML file into g_wpkg[]. */
static void parse_widget_xml(FILE *f) {
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while ((p = strstr(p, "pkg=\"")) != NULL) {
            p += 5; char *end = strchr(p, '"'); if (!end) break;
            ptrdiff_t span = end - p;
            if (span <= 0 || span >= 128) { p = end+1; continue; }
            size_t len = (size_t)span;
            bool dup = false;
            for (int j = 0; j < g_wpkg_count && !dup; j++)
                dup = (strncmp(g_wpkg[j], p, len) == 0 && g_wpkg[j][len] == '\0');
            if (!dup && g_wpkg_count < 64) {
                strncpy(g_wpkg[g_wpkg_count], p, len);
                g_wpkg[g_wpkg_count][len] = '\0';
                g_wpkg_count++;
            }
            p = end + 1;
        }
    }
}

static void load_active_widget_pkgs(void) {
    refresh_dynamic_protected_pkgs();
    time_t now = time(NULL);
    int intvl = (g_wpkg_count > 0) ? WIDGET_RELOAD_S : WIDGET_RETRY_S;
    if (g_wpkg_ts > 0 && now - g_wpkg_ts < intvl) return;
    g_wpkg_ts = now;

    /* Candidate XML paths: system-level + per-user (users/0 .. users/9) */
    const char *primary_paths[] = {
        APPWIDGET_XML, APPWIDGET_XML_U0, NULL
    };
    char extra[10][64];
    const char *extra_ptrs[11]; int ne = 0;
    for (int u = 1; u <= 9; u++) {
        snprintf(extra[ne], sizeof(extra[ne]),
                 "/data/system/users/%d/appwidgets.xml", u);
        struct stat st;
        if (stat(extra[ne], &st) == 0) { extra_ptrs[ne] = extra[ne]; ne++; }
    }
    extra_ptrs[ne] = NULL;

    /* Try all paths until at least one loads */
    bool loaded_any = false;
    int tmp_count = 0;
    char tmp_pkgs[64][128] = {{0}};

    /* Inline parse into tmp buffer first so we don't clobber on partial read */
    for (int pi = 0; primary_paths[pi]; pi++) {
        FILE *f = fopen(primary_paths[pi], "r");
        if (!f) continue;
        int save = g_wpkg_count;
        g_wpkg_count = tmp_count;
        memcpy(g_wpkg, tmp_pkgs, (size_t)tmp_count * 128);
        parse_widget_xml(f);
        fclose(f);
        tmp_count = g_wpkg_count;
        memcpy(tmp_pkgs, g_wpkg, (size_t)tmp_count * 128);
        g_wpkg_count = save;
        loaded_any = true;
    }
    for (int pi = 0; extra_ptrs[pi]; pi++) {
        FILE *f = fopen(extra_ptrs[pi], "r");
        if (!f) continue;
        int save = g_wpkg_count;
        g_wpkg_count = tmp_count;
        memcpy(g_wpkg, tmp_pkgs, (size_t)tmp_count * 128);
        parse_widget_xml(f);
        fclose(f);
        tmp_count = g_wpkg_count;
        memcpy(tmp_pkgs, g_wpkg, (size_t)tmp_count * 128);
        g_wpkg_count = save;
        loaded_any = true;
    }

    /* Fallback: cmd appwidget list (Android 7+) */
    if (!loaded_any || tmp_count == 0) {
        FILE *cmd = popen("cmd appwidget list 2>/dev/null", "r");
        if (cmd) {
            char buf[256];
            while (fgets(buf, sizeof(buf), cmd)) {
                /* Lines look like: "package=com.example.widget ..." */
                char *kw = strstr(buf, "package=");
                if (!kw) kw = strstr(buf, "pkg=");
                if (!kw) continue;
                kw += (kw[1] == 'k') ? 4 : 8; /* skip "pkg=" or "package=" */
                char pkg[128]; int n = 0;
                while (kw[n] && kw[n] != ' ' && kw[n] != '\n' && n < 127) {
                    pkg[n] = kw[n]; n++;
                }
                pkg[n] = '\0';
                if (n > 0 && tmp_count < 64) {
                    bool dup = false;
                    for (int j = 0; j < tmp_count && !dup; j++)
                        dup = strcmp(tmp_pkgs[j], pkg) == 0;
                    if (!dup) { strncpy(tmp_pkgs[tmp_count++], pkg, 127); loaded_any = true; }
                }
            }
            pclose(cmd);
        }
    }

    if (!loaded_any) {
        logi("Widget: no source readable – retry in %ds", WIDGET_RETRY_S);
        return;
    }

    /* Compare as a set: XML/source ordering is not meaningful. Invalidate
     * cached static classifications only when provider membership changes. */
    bool changed = (tmp_count != g_wpkg_count);
    for (int i = 0; i < tmp_count && !changed; i++) {
        bool found = false;
        for (int j = 0; j < g_wpkg_count; j++)
            if (strcmp(tmp_pkgs[i], g_wpkg[j]) == 0) { found = true; break; }
        if (!found) changed = true;
    }

    /* Commit: only update the live list once we have a result */
    g_wpkg_count = tmp_count;
    memcpy(g_wpkg, tmp_pkgs, (size_t)tmp_count * 128);
    if (changed) {
        for (int i = 0; i < g_score_count; i++)
            g_scores[i].static_class_cache = -1;
        logi("Widget: provider list changed; refreshed static class cache");
    }
    if (g_wpkg_count > 0)
        logi("Widget: %d active provider(s) loaded", g_wpkg_count);
}

static bool is_active_widget_provider(const char *name) {
    for (int i = 0; i < g_wpkg_count; i++)
        if (g_wpkg[i][0] && strstr(name, g_wpkg[i])) return true;
    return false;
}

/* Foreground history */
static void update_fg_history(const char *name, time_t now) {
    if (!name[0]) return;
    for (int i = 0; i < g_fg_history_count; i++) {
        if (strcmp(g_fg_history[i].name, name) == 0) {
            g_fg_history[i].last_seen = now; return;
        }
    }
    if (g_fg_history_count < FG_HISTORY_SIZE) {
        strncpy(g_fg_history[g_fg_history_count].name, name,
                sizeof(g_fg_history[0].name)-1);
        g_fg_history[g_fg_history_count].last_seen = now;
        g_fg_history_count++;
    } else {
        int oldest = 0;
        for (int i = 1; i < FG_HISTORY_SIZE; i++)
            if (g_fg_history[i].last_seen < g_fg_history[oldest].last_seen)
                oldest = i;
        strncpy(g_fg_history[oldest].name, name, sizeof(g_fg_history[0].name)-1);
        g_fg_history[oldest].last_seen = now;
    }
}

static time_t get_last_fg(const char *name) {
    for (int i = 0; i < g_fg_history_count; i++)
        if (strcmp(g_fg_history[i].name, name) == 0)
            return g_fg_history[i].last_seen;
    return 0;
}

/* Kill history + cooldown */
static int kill_cooldown_for(const char *name) {

    AppScore *s = score_lookup(name);
    if (s) {
            int rc = s->restart_count;
            int cd = RESTART_CD_BASE_S + rc * RESTART_CD_SCALE_S;

            int cap = (rc >= BOUNCE_SUPPRESS_RC) ? BOUNCE_KILL_CD_MAX_S
                                                  : RESTART_CD_MAX_S;
            return cd > cap ? cap : cd;
    }
    return RESTART_CD_BASE_S;
}

static bool recently_killed(const char *name) {
    time_t now = time(NULL);
    int cooldown = kill_cooldown_for(name);
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (g_kill_hist[i].name[0] &&
            strcmp(g_kill_hist[i].name, name) == 0 &&
            (now - g_kill_hist[i].killed_at) < cooldown)
            return true;
    }
    return false;
}

static AppScore *score_lookup(const char *name);
static int score_compute(const AppScore *s);

static void record_kill(ProcInfo *p) {
    time_t now = time(NULL);
    const char *name = p->name;

    kswapd_note_disruptive_event();

    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (strcmp(g_kill_hist[i].name, name) == 0) {
            g_kill_hist[i].killed_at = now;
            g_kill_hist[i].bounced   = false;
            g_kill_hist[i].scheduled = g_reset_in_progress;
            return;
        }
    }
    strncpy(g_kill_hist[g_kill_hist_idx].name, name, 255);
    g_kill_hist[g_kill_hist_idx].name[255]   = '\0';
    g_kill_hist[g_kill_hist_idx].killed_at   = now;
    g_kill_hist[g_kill_hist_idx].bounced     = false;
    g_kill_hist[g_kill_hist_idx].scheduled   = g_reset_in_progress;
    g_kill_hist_idx = (g_kill_hist_idx + 1) % KILL_HIST_SIZE;
}

static void check_bounced(const char *name) {
    time_t now = time(NULL);
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (g_kill_hist[i].name[0] &&
            strcmp(g_kill_hist[i].name, name) == 0 &&
            !g_kill_hist[i].bounced &&
            (now - g_kill_hist[i].killed_at) < RESTART_WINDOW_S) {
            g_kill_hist[i].bounced = true;
            if (g_kill_hist[i].scheduled) {

                continue;
            }
            for (int j = 0; j < g_score_count; j++) {
                if (strcmp(g_scores[j].name, name) == 0) {
                    g_scores[j].restart_count++;
                    g_scores[j].last_restart_t = now;
                    g_scores[j].dirty = true;
                    g_scores_dirty = true;
                    logi("Resilient: %s restart_count=%d (cd=%ds)",
                         name, g_scores[j].restart_count,
                         kill_cooldown_for(name));
                    break;
                }
            }
            break;
        }
    }
}

/* System/user package cache from PackageManager: both lists (-s, -3) are cached at once, so classify_dumpsys_drain() confirms nearly every package
 * without a per-name `cmd package path` (a fork per name). This replaces hardcoded prefixes, which miss OEM packages (com.miui.*, com.samsung.*). */
#define SYSPKG_CACHE_S 300
static char **g_syspkg_list  = NULL;
static int    g_syspkg_count = 0;
static char **g_userpkg_list = NULL;
static int    g_userpkg_count = 0;
static time_t g_syspkg_ts    = 0;

static void pkg_list_free(char **list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

/* Run one `cmd package list packages <flag>` and return a malloc'd name list; on failure *out_list stays NULL and 0 is returned so a previously cached list is kept. */
static int pkg_list_load(const char *cmd, char ***out_list) {
    *out_list = NULL;
    FILE *p = popen(cmd, "r");
    if (!p) return 0;

    int cap = 256, n = 0;
    char **list = calloc((size_t)cap, sizeof(char *));
    if (!list) { pclose(p); return 0; }

    char line[256];
    while (fgets(line, sizeof(line), p)) {
        if (strncmp(line, "package:", 8) != 0) continue;
        int len = (int)strlen(line + 8);
        while (len > 0 && (line[8+len-1]=='\n' || line[8+len-1]=='\r' || line[8+len-1]==' '))
            len--;
        if (len <= 0) continue;
        if (n >= cap) {
            /* cap is always > 0 here; the explicit ceiling keeps cap*2 far from int overflow. */
            if (cap >= (1 << 20)) break;
            cap *= 2;
            char **nl = realloc(list, (size_t)cap * sizeof(char *));
            if (!nl) break;
            list = nl;
        }
        list[n] = strndup(line + 8, (size_t)len);
        if (list[n]) n++;
    }
    pclose(p);

    if (n == 0) { free(list); return 0; }
    *out_list = list;
    return n;
}

static void load_syspkg_cache(void) {
    time_t now = time(NULL);
    if (g_syspkg_ts > 0 && (now - g_syspkg_ts) < SYSPKG_CACHE_S) return;
    g_syspkg_ts = now;   /* set before popen so a failure doesn't hammer */

    char **sys_list  = NULL; int sys_n  = 0;
    char **user_list = NULL; int user_n = 0;
    sys_n  = pkg_list_load("cmd package list packages -s 2>/dev/null", &sys_list);
    user_n = pkg_list_load("cmd package list packages -3 2>/dev/null", &user_list);

    if (sys_n == 0 && user_n == 0) {
        pkg_list_free(sys_list, sys_n);
        pkg_list_free(user_list, user_n);
        return;   /* keep whatever cache we already had */
    }

    /* Swap in each list that succeeded; keep the old copy of a list that
     * failed this round rather than dropping a still-useful cache. */
    if (sys_n > 0) {
        pkg_list_free(g_syspkg_list, g_syspkg_count);
        g_syspkg_list  = sys_list;
        g_syspkg_count = sys_n;
    } else {
        pkg_list_free(sys_list, sys_n);
    }
    if (user_n > 0) {
        pkg_list_free(g_userpkg_list, g_userpkg_count);
        g_userpkg_list  = user_list;
        g_userpkg_count = user_n;
    } else {
        pkg_list_free(user_list, user_n);
    }
    logi("SysPkg: %d system + %d user package di-cache via cmd package",
         g_syspkg_count, g_userpkg_count);
}

/* Returns 0 not listed, 1 system, 2 user. An exact match beats a parent-package match so a separately installed subpackage keeps its own class. */
static int pkg_cache_lookup(const char *base) {
    if (!base || !base[0]) return 0;
    for (int i = 0; i < g_syspkg_count; i++)
        if (strcmp(base, g_syspkg_list[i]) == 0) return 1;
    for (int i = 0; i < g_userpkg_count; i++)
        if (strcmp(base, g_userpkg_list[i]) == 0) return 2;

    char cand[256];
    strncpy(cand, base, 255); cand[255] = '\0';
    for (;;) {
        char *dot = strrchr(cand, '.');
        if (!dot) return 0;
        *dot = '\0';
        for (int i = 0; i < g_syspkg_count; i++)
            if (strcmp(cand, g_syspkg_list[i]) == 0) return 1;
        for (int i = 0; i < g_userpkg_count; i++)
            if (strcmp(cand, g_userpkg_list[i]) == 0) return 2;
    }
}

static bool is_system_package_cached(const char *name) {
    if (g_syspkg_count == 0 && g_userpkg_count == 0) load_syspkg_cache();
    if (g_syspkg_count == 0 && g_userpkg_count == 0) return false;

    /* Strip the ':' suffix (process alias) */
    char base[256];
    strncpy(base, name, 255); base[255] = '\0';
    char *colon = strchr(base, ':');
    if (colon) *colon = '\0';

    return pkg_cache_lookup(base) == 1;
}

/* Score category classifier */
static int classify_score_cat(const char *name) {
    if (strchr(name, '/')) return 0;                 /* native path */

    static const char * const NATIVE_NAMES[] = {
        "zygote","zygote64","webview_zygote","system_server",
        "magiskd","logcat","top","su","lmk_engine",
        "<pre-initialized>",
        NULL
    };
    for (int i = 0; NATIVE_NAMES[i]; i++)
        if (!strcmp(name, NATIVE_NAMES[i])) return 0;
    if (strncmp(name, "media.", 6) == 0) return 0;

    /* A name with no dot and no ':' is a native binary/daemon */
    {
        char base[256]; strncpy(base, name, 255); base[255] = '\0';
        char *colon = strchr(base, ':'); if (colon) *colon = '\0';
        if (!strchr(base, '.')) return 0;
    }

    /* Main signal: the PackageManager SYSTEM flag (universal, accurate) */
    if (is_system_package_cached(name)) return 1;

    /* Heuristic fallback only for packages not yet in the cache (e.g. a system app updated before the cache reload) */
    if (strncmp(name, "com.android.",  12) == 0) return 1;
    if (strncmp(name, "android.",       8) == 0) return 1;
    return 2;
}

/* App usage scoring */

#define SCORE_HASH_SIZE 512  /* power of two, > 2x SCORE_MAX_APPS */
static int  g_score_hash[SCORE_HASH_SIZE];
static bool g_score_hash_dirty = true;

static unsigned score_hash_str(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)(*s++); h *= 16777619u; }
    return h;
}

static void score_hash_rebuild(void) {
    for (int i = 0; i < SCORE_HASH_SIZE; i++) g_score_hash[i] = -1;
    for (int i = 0; i < g_score_count; i++) {
        unsigned h = score_hash_str(g_scores[i].name) & (SCORE_HASH_SIZE - 1);
        while (g_score_hash[h] != -1) h = (h + 1) & (SCORE_HASH_SIZE - 1);
        g_score_hash[h] = i;
    }
    g_score_hash_dirty = false;
}

static AppScore *score_lookup(const char *name) {
    if (g_score_hash_dirty) score_hash_rebuild();
    unsigned h = score_hash_str(name) & (SCORE_HASH_SIZE - 1);
    for (int probes = 0; probes < SCORE_HASH_SIZE; probes++) {
        int idx = g_score_hash[h];
        if (idx == -1) return NULL;
        if (strcmp(g_scores[idx].name, name) == 0) return &g_scores[idx];
        h = (h + 1) & (SCORE_HASH_SIZE - 1);
    }
    return NULL;
}

static int score_compute(const AppScore *s) {
    if (!s || s->raw_last_fg_do_not_read_directly == 0) return 0;
    time_t now = time(NULL);
    long idle_h = (now - s->raw_last_fg_do_not_read_directly) / 3600;
    long idle_d = idle_h / 24;

    int raw_ticks = s->fg_count < FG_COUNT_MAX ? s->fg_count : FG_COUNT_MAX;
    int ticks = raw_ticks / FG_COUNT_SCALE;
    if (ticks > SCORE_MAX) ticks = SCORE_MAX;
    int steps = (int)(idle_h / 12);
    for (int i = 0; i < steps && ticks > 0; i++) ticks >>= 1;

    /* Signal 2: session frequency bonus with decay per day */
    int session_count = s->session_count;
    for (int d = 0; d < idle_d && session_count > 0; d++) session_count >>= 1;
    int freq_bonus = 0;
    if (session_count > 0) {
        long window_h = idle_h > 168 ? 168 : (idle_h < 1 ? 1 : idle_h);
        freq_bonus = (int)((long)session_count * 240 / window_h);
        if (freq_bonus > 400) freq_bonus = 400;
    }

    /* Signal 3: recency bonus */
    int recency = 0;
    if      (idle_h == 0)  recency = 100;
    else if (idle_h < 4)   recency =  50;
    else if (idle_h < 24)  recency =  20;

    int duration_bonus = 0;
    if (s->avg_session_duration_s > 0) {
        long capped = s->avg_session_duration_s > SESSION_DURATION_CAP_S
                      ? SESSION_DURATION_CAP_S : s->avg_session_duration_s;
        duration_bonus = (int)(capped * SESSION_DURATION_BONUS_MAX / SESSION_DURATION_CAP_S);
    }

    int sc = ticks + freq_bonus + recency + duration_bonus;

    return sc > SCORE_MAX ? SCORE_MAX : (sc < 0 ? 0 : sc);
}

static int score_get(const char *name) {
    AppScore *s = score_lookup(name);
    return s ? score_compute(s) : 0;
}

static int score_protect_window_ram(const char *name, int avail_pct) {
    int sc  = score_get(name);

    int win = FG_PROTECT_BASE + (int)((long)sc * FG_PROTECT_MAX / SCORE_MAX);
    if (win > FG_PROTECT_MAX) win = FG_PROTECT_MAX;

    if (avail_pct < FREE_CRIT_PCT)  return FG_PROTECT_CRIT_S;
    if (avail_pct < FREE_LOW_PCT)   return win * FG_PROTECT_LOW_PCT / 100;

    if (g_zram_used_pct > 0 && g_zram_used_pct < (ZRAM_WARN_PCT - 15))
        win = win * 130 / 100;
    else if (g_zram_used_pct < ZRAM_WARN_PCT)
        win = win * 100 / 100;         /* no discount below WARN_PCT */
    else if (g_zram_used_pct < ZRAM_TRIM_PCT)
        win = win * 80 / 100;
    else if (g_zram_used_pct < ZRAM_STUCK_PCT)
        win = win * 50 / 100;
    else
        win = win * 20 / 100;          /* was ~8% compounded; now a clean 20% */
    if (win > FG_PROTECT_MAX) win = FG_PROTECT_MAX;

    return win;
}

static int score_protect_window(const char *name) {
    return score_protect_window_ram(name, FREE_HIGH_PCT);
}

static bool is_actively_retained(const char *name) {
    AppScore *s = score_lookup(name);
    return s && s->retain_pin_t > 0 &&
           (time(NULL) - s->retain_pin_t) < RETAIN_PROTECT_GRACE_S;
}

typedef struct {
    char name[256];
    int  attempts;
} SysCheckEntry;
static SysCheckEntry g_syscheck_queue[SYSCHECK_QUEUE_MAX];
static int  g_syscheck_queue_n = 0;

/* Queue the name with its ":suffix" stripped (aliases share the base package record); dot suffixes stay, e.g. gms.ui is its own package. */
static void classify_queue_maybe(const char *name) {
    char base[256];
    strncpy(base, name, 255); base[255] = '\0';
    char *colon = strchr(base, ':');
    if (colon) *colon = '\0';
    if (!base[0] || strchr(base, '/')) return; /* native binaries, not packages */
    if (!strchr(base, '.')) return; /* dot-less = bare command name, already
                                       * classified native by classify_score_cat() */

    for (int i = 0; i < g_syscheck_queue_n; i++)
        if (!strcmp(g_syscheck_queue[i].name, base)) return; /* already queued */
    if (g_syscheck_queue_n >= SYSCHECK_QUEUE_MAX) {
        logw("Classify: queue full (%d), dropping [%s]", SYSCHECK_QUEUE_MAX, base);
        return;
    }

    strncpy(g_syscheck_queue[g_syscheck_queue_n].name, base, 255);
    g_syscheck_queue[g_syscheck_queue_n].name[255] = '\0';
    g_syscheck_queue[g_syscheck_queue_n].attempts = 0;
    g_syscheck_queue_n++;
}

/* Internal: requeue with a preserved attempt count (used on retry, unlike
 * classify_queue_maybe() which is for brand-new names only). */
static void classify_queue_requeue(const char *name, int attempts) {
    for (int i = 0; i < g_syscheck_queue_n; i++)
        if (!strcmp(g_syscheck_queue[i].name, name)) return; /* already queued */
    if (g_syscheck_queue_n >= SYSCHECK_QUEUE_MAX) {
        logw("Classify: queue full (%d), dropping [%s] on retry", SYSCHECK_QUEUE_MAX, name);
        return;
    }
    strncpy(g_syscheck_queue[g_syscheck_queue_n].name, name, 255);
    g_syscheck_queue[g_syscheck_queue_n].name[255] = '\0';
    g_syscheck_queue[g_syscheck_queue_n].attempts = attempts;
    g_syscheck_queue_n++;
}

/* Android package and process names: alphanumeric start, then letters,
 * digits and `. _ : -`. Anything else (spaces, quotes, `;`, `$`, backticks,
 * a leading `-`) is rejected before it can reach an argv. */
static bool pkg_name_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 255 || !isalnum((unsigned char)s[0])) return false;
    for (size_t i = 1; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-')) return false;
    }
    return true;
}

#define EXEC_CAPTURE_TIMEOUT_MS 5000

/* Run argv[0] (PATH lookup) without a shell and capture stdout.
 * Returns 0 with *exit_code set, -1 if fork/pipe/exec failed, -2 on timeout
 * (child killed and reaped). out is always NUL-terminated. */
static int exec_capture(char *const argv[], char *out, size_t outsz, int *exit_code) {
    out[0] = '\0';
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); dup2(nul, STDERR_FILENO); }
        dup2(pfd[1], STDOUT_FILENO);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t used = 0;
    int rc = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (elapsed >= EXEC_CAPTURE_TIMEOUT_MS) { rc = -2; break; }
        struct pollfd pf = { .fd = pfd[0], .events = POLLIN };
        int pr = poll(&pf, 1, (int)(EXEC_CAPTURE_TIMEOUT_MS - elapsed));
        if (pr < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (pr == 0) { rc = -2; break; }
        char buf[512];
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (n == 0) break;
        size_t room = outsz - 1 - used;
        size_t take = (size_t)n < room ? (size_t)n : room;
        memcpy(out + used, buf, take);
        used += take;
    }
    out[used] = '\0';
    close(pfd[0]);
    if (rc != 0) kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    if (rc != 0) return rc;
    *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return *exit_code == 127 ? -1 : 0;   /* 127: exec failed in the child */
}

typedef enum { CT_OK, CT_NOT_FOUND, CT_INVALID_NAME, CT_EXEC_FAILED } ClassifyTry;

/* Resolve system/user from the APK location reported by
 * `cmd package path <pkg>` (fallback `pm path <pkg>`). The name is passed
 * as a separate argv entry, never through a shell. */
static ClassifyTry classify_try(const char *pkg, bool *is_system) {
    *is_system = false;
    if (!pkg_name_valid(pkg)) return CT_INVALID_NAME;

    char name[512];
    snprintf(name, sizeof(name), "%s", pkg);
    char *const cmd_argv[] = { "cmd", "package", "path", name, NULL };
    char *const pm_argv[]  = { "pm", "path", name, NULL };
    char *const *tries[]   = { cmd_argv, pm_argv };
    bool any_ran = false;

    for (size_t t = 0; t < 2; t++) {
        char out[2048];
        int code = 0;
        if (exec_capture(tries[t], out, sizeof(out), &code) != 0) continue;
        any_ran = true;
        if (code != 0) continue;
        bool parsed = false;
        char *saveptr = NULL;
        for (char *line = strtok_r(out, "\r\n", &saveptr);
             line != NULL;
             line = strtok_r(NULL, "\r\n", &saveptr)) {
            if (strncmp(line, "package:", 8) != 0) continue;
            parsed = true;
            const char *path = line + 8;
            if (strstr(path, "/system/")     || strstr(path, "/system_ext/") ||
                strstr(path, "/product/")    || strstr(path, "/vendor/")     ||
                strstr(path, "/odm/")        || strstr(path, "/apex/")) {
                *is_system = true;
                break;
            }
        }
        if (parsed) return CT_OK;
    }
    return any_ran ? CT_NOT_FOUND : CT_EXEC_FAILED;
}

/* Apply a resolved category to every tracked entry whose colon-stripped
 * name matches pkg exactly (fixes alias-only packages like gearhead,
 * which only ever exist as ":car"/":provider", never a bare entry). */
static int classify_apply(const char *pkg, int category) {
    int applied = 0;
    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        char ename[256];
        strncpy(ename, s->name, 255); ename[255] = '\0';
        char *ecolon = strchr(ename, ':');
        if (ecolon) *ecolon = '\0';
        if (strcmp(ename, pkg) != 0) continue;

        s->category = category;
        s->category_confirmed = true;
        s->category_confirmed_at = time(NULL);
        s->dirty = true;
        g_scores_dirty = true;
        applied++;
    }
    return applied;
}

/* Drains up to SYSCHECK_DRAIN_PER_TICK names per call while screen-off >= CLASSIFY_IDLE_S, using `dumpsys package`. An unresolved name walks up its dot segments (gms.ui -> gms);
 * after SYSCHECK_MAX_ATTEMPTS failures it falls back to classify_score_cat() and is marked confirmed. */
static void classify_dumpsys_drain(void) {
    /* Decoupled from the IDLE-DEEP tier (tier==-3, 30 min), a dumpsys
     * lookup is cheap and one-off, so it only needs the screen to have
     * been off CLASSIFY_IDLE_S (7 min), not the full conservative bar
     * that kill-tier escalation uses. */
    if (g_screen_off_since == 0) return;
    if ((time(NULL) - g_screen_off_since) < CLASSIFY_IDLE_S) return;
    int drained = 0;
    while (g_syscheck_queue_n > 0 && drained < SYSCHECK_DRAIN_PER_TICK) {
        char pkg[256]; int attempts = g_syscheck_queue[0].attempts;
        strncpy(pkg, g_syscheck_queue[0].name, 255); pkg[255] = '\0';
        for (int i = 1; i < g_syscheck_queue_n; i++)
            g_syscheck_queue[i - 1] = g_syscheck_queue[i];
        g_syscheck_queue_n--;
        drained++;

        char candidate[256];
        strncpy(candidate, pkg, 255); candidate[255] = '\0';
        bool parsed = false, is_system = false;
        const char *via = "";

        /* Cached package lists first, no fork: with both lists in memory almost every name resolves here (pkg_cache_lookup dot-walks itself). */
        int cached = pkg_cache_lookup(pkg);
        if (cached == 1 || cached == 2) {
            parsed    = true;
            is_system = (cached == 1);
            via       = " via cached package list";
        } else {
            /* Fallback: `cmd package path` with dot-walking, only for names absent from both lists. */
            ClassifyTry ct;
            for (;;) {
                ct = classify_try(candidate, &is_system);
                parsed = (ct == CT_OK);
                if (parsed || ct == CT_INVALID_NAME) break;
                char *dot = strrchr(candidate, '.');
                if (!dot) break; /* nothing shorter left to try */
                *dot = '\0';
            }
            if (ct == CT_INVALID_NAME) {
                /* Retrying cannot make a malformed name valid. */
                int fallback = classify_score_cat(pkg);
                int applied = classify_apply(pkg, fallback);
                logw("Classify: rejected invalid package name [%.64s], fallback "
                     "category=%d, applied to %d entr%s",
                     pkg, fallback, applied, applied == 1 ? "y" : "ies");
                continue;
            }
            if (ct == CT_EXEC_FAILED)
                logw("Classify: cannot run cmd/pm for [%s], will retry", pkg);
            if (parsed && strcmp(candidate, pkg))
                via = " via parent lookup";
        }

        if (parsed) {
            int applied = classify_apply(pkg, is_system ? 1 : 2);
            if (applied > 0) {
                logi("Classify: [%s] confirmed category=%d (SYSTEM=%s)%s, applied to %d entr%s",
                     pkg, is_system ? 1 : 2, is_system ? "yes" : "no",
                     via, applied, applied == 1 ? "y" : "ies");
            } else {
                logw("Classify: [%s] confirmed category=%d (SYSTEM=%s) but no matching entries found",
                     pkg, is_system ? 1 : 2, is_system ? "yes" : "no");
            }
            continue;
        }

        attempts++;
        if (attempts >= SYSCHECK_MAX_ATTEMPTS) {
            int fallback = classify_score_cat(pkg);
            int applied = classify_apply(pkg, fallback);
            logw("Classify: giving up on [%s] after %d attempts (no dumpsys match at any level), "
                 "using fallback category=%d, applied to %d entr%s",
                 pkg, attempts, fallback, applied, applied == 1 ? "y" : "ies");
        } else {
            logw("Classify: no parseable dumpsys output for [%s] (attempt %d/%d), requeueing",
                 pkg, attempts, SYSCHECK_MAX_ATTEMPTS);
            classify_queue_requeue(pkg, attempts);
        }
    }
}

static void category_reconfirm_sweep(void) {
    static time_t last_sweep_t = 0;
    time_t now = time(NULL);

    if (last_sweep_t == 0) { last_sweep_t = now; return; }
    if ((now - last_sweep_t) < CATEGORY_RECONFIRM_S) return;
    last_sweep_t = now;

    int requeued = 0;
    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        if (!s->category_confirmed) continue;
        /* confirmed_at == 0 means unknown/legacy (pre-v9 or never stamped): eligible immediately, like first_seen == 0; otherwise re-check only past the window. */
        if (s->category_confirmed_at != 0 &&
            (now - s->category_confirmed_at) < CATEGORY_RECONFIRM_S) continue;
        s->category_confirmed = false;
        s->dirty = true;
        g_scores_dirty = true;
        classify_queue_maybe(s->name);
        requeued++;
    }
    if (requeued > 0)
        logi("CategoryReconfirm: re-queued %d stale/unverified entr%s for a fresh dumpsys check",
             requeued, requeued == 1 ? "y" : "ies");
}

/* Shared with score_touch(): once SCORE_MAX_APPS is full, reuse the lowest-scoring entry so new services aren't locked out by stale ones. */
#define EVICT_CONFIRMED_BONUS 80

static bool cgroup_write(pid_t pid, const char *file, const char *val);

/* Eviction drops the in-memory cg_high_set/cg_low_set flags, so restore memory.high/memory.low defaults for any live process of this name first;
 * otherwise the squeeze stays in force forever. */
static void score_evict_release_cgroups(AppScore *victim) {
    if (!victim || !victim->name[0]) return;
    if (!victim->cg_high_set && !victim->cg_low_set) return;

    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *de;
    char pname[256];
    int released = 0;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 1) continue;
        /* Same name source as enumerate_procs() (first token of /proc/<pid>/cmdline), so it matches the processes the flags were set on. */
        if (proc_cmdline(pid, pname, sizeof(pname)) != 0 || !pname[0]) continue;
        if (strcmp(pname, victim->name) != 0) continue;
        if (victim->cg_high_set &&
            cgroup_write(pid, "memory.high", "max")) released++;
        if (victim->cg_low_set &&
            cgroup_write(pid, "memory.low", "0")) released++;
    }
    closedir(d);
    if (released > 0)
        logi("ScoreEvict: restored memory.high/memory.low defaults on [%s] "
             "(%d cgroup write(s)) before dropping the entry",
             victim->name, released);
}

static AppScore *score_alloc_slot(void) {
    if (g_score_count < SCORE_MAX_APPS)
        return &g_scores[g_score_count++];

    int min_idx = 0;
    int min_key = score_compute(&g_scores[0]) +
                  (g_scores[0].category_confirmed ? EVICT_CONFIRMED_BONUS : 0);
    for (int i = 1; i < g_score_count; i++) {
        int key = score_compute(&g_scores[i]) +
                  (g_scores[i].category_confirmed ? EVICT_CONFIRMED_BONUS : 0);
        if (key < min_key) { min_key = key; min_idx = i; }
    }

    logw("ScoreEvict: [%s] evicted for a new entry (score=%d confirmed=%d "
         "fg_count=%d table=%d/%d)",
         g_scores[min_idx].name, min_key, g_scores[min_idx].category_confirmed ? 1 : 0,
         g_scores[min_idx].fg_count, g_score_count, SCORE_MAX_APPS);
    /* The caller memsets this slot on return, so release any live cgroup limit first while the flags are still readable. */
    score_evict_release_cgroups(&g_scores[min_idx]);
    return &g_scores[min_idx];
}

/* Look up an AppScore entry, creating it if missing; shared by score_touch() and track_foreground(), which needs an entry for fg_hold_start before a session is credited. */
static AppScore *score_lookup_or_create(const char *name) {
    AppScore *s = score_lookup(name);
    if (s) return s;

    s = score_alloc_slot();
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 255); s->name[255] = '\0';
    s->static_class_cache = -1;
    s->first_seen = time(NULL);
    s->dirty = true;
    g_scores_dirty = true;
    g_score_hash_dirty = true;

    int guess = classify_score_cat(name);
    s->category = guess;
    if (guess != 2) {
        s->category_confirmed = true;
        s->category_confirmed_at = time(NULL);
    } else {
        classify_queue_maybe(name);
    }
    return s;
}

static void score_touch(const char *name, time_t now, long swap_kb) {
    if (!name[0]) return;
    AppScore *s = score_lookup_or_create(name);

    bool new_session = (s->raw_last_fg_do_not_read_directly == 0 ||
                        (now - s->raw_last_fg_do_not_read_directly) >= SESSION_GAP_S);
    if (new_session && s->session_count < 99999) {
        s->session_count++;
        s->dirty = true;
        g_scores_dirty = true;   /* session boundary is worth persisting */
    }

    if (new_session) {
        if (s->session_start_t != 0) {
            long dur = (long)(s->raw_last_fg_do_not_read_directly - s->session_start_t);
            if (dur > 0) {
                long old_avg = s->avg_session_duration_s;
                s->avg_session_duration_s = (s->avg_session_duration_s * (100 - SCORE_AVG_ALPHA_PCT) +
                                              dur * SCORE_AVG_ALPHA_PCT) / 100;
                if (labs(s->avg_session_duration_s - old_avg) >= 5) {
                    s->dirty = true;
                    g_scores_dirty = true;
                }
            }
        }
        s->session_start_t = now;
    }

    s->raw_last_fg_do_not_read_directly = now;
    s->last_genuine_fg = now;
    if (s->fg_count < FG_COUNT_MAX) s->fg_count++;

    if (swap_kb >= 0) {
        long old_avg = s->avg_swap_kb;
        s->avg_swap_kb = (s->avg_swap_kb * (100 - SCORE_AVG_ALPHA_PCT) +
                          swap_kb * SCORE_AVG_ALPHA_PCT) / 100;
        /* Only a meaningful swap move is worth persisting, not per-tick EMA noise. */
        if (labs(s->avg_swap_kb - old_avg) >= 1024) {
            s->dirty = true;
            g_scores_dirty = true;
        }
    }
}

/* Updates recency only for background-visible launches; fg_count/session_count stay a measure of real user engagement. */
static void score_touch_bg(const char *name, time_t now, long swap_kb) {
    if (!name[0]) return;
    AppScore *s = score_lookup(name);
    if (!s) return; /* don't create new score entries for bg-only activity */
    s->raw_last_fg_do_not_read_directly = now;   /* in-memory recency refresh only, see score_touch() */
    if (swap_kb >= 0) {
        long old_avg = s->avg_swap_kb;
        s->avg_swap_kb = (s->avg_swap_kb * (100 - SCORE_AVG_ALPHA_PCT) +
                          swap_kb * SCORE_AVG_ALPHA_PCT) / 100;
        if (labs(s->avg_swap_kb - old_avg) >= 1024) {
            s->dirty = true;
            g_scores_dirty = true;
        }
    }
}

/* Evaluate one process against its persisted growth-watch window and update strike/flag state; called per tick for every non-exempt process. */
static void check_runaway_growth(const char *name, time_t now, long rss_kb) {
    if (!name[0] || rss_kb < GROWTH_MIN_BASE_KB) return;
    AppScore *s = score_lookup(name);
    if (!s) return;

    if (s->rss_watch_t == 0) {
        s->rss_watch_kb = rss_kb;
        s->rss_watch_t  = now;
        return;
    }
    if (now - s->rss_watch_t < GROWTH_WINDOW_S) return;

    long base = s->rss_watch_kb > 0 ? s->rss_watch_kb : 1;
    long grew_pct = ((rss_kb - base) * 100) / base;

    if (grew_pct >= GROWTH_STRIKE_PCT) {
        s->rss_growth_strikes++;
    } else {
        s->rss_growth_strikes = 0;
        s->flagged_runaway    = false;
    }

    if (s->rss_growth_strikes >= GROWTH_STRIKES_FLAG) {
        if (!s->flagged_runaway || (now - s->runaway_alert_t) >= GROWTH_REALERT_S) {
            logw("[GROWTH] '%s' looks like a RUNAWAY: %ldMB -> %ldMB over ~%dm "
                 "(%d consecutive growth windows) — prioritizing for kill",
                 name, base/1024, rss_kb/1024,
                 (int)((now - (s->rss_watch_t - GROWTH_WINDOW_S)) / 60),
                 s->rss_growth_strikes);
            s->runaway_alert_t = now;
        }
        s->flagged_runaway = true;
    }

    /* Slide the window forward regardless of outcome */
    s->rss_watch_kb = rss_kb;
    s->rss_watch_t  = now;
}

static int g_score_journal_pending = 0;

/* Base and journal are linked by seq: a journal is replayed only when its seq matches the base.
 * g_score_journal_ok false forces the next save to be a full rewrite. */
static unsigned long g_score_seq = 0;
static bool g_score_journal_ok = false;

/* Flush, fsync and close; false if any step failed, stream closed either way. EINVAL/EROFS from fsync mean the fs can't sync and aren't data loss. */
static bool stdio_commit(FILE *f) {
    bool ok = fflush(f) == 0;
    if (ok && fsync(fileno(f)) != 0 && errno != EINVAL && errno != EROFS) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Best effort: make a completed rename survive a crash or power loss. */
static void fsync_parent_dir(const char *path) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir) dir[1] = '\0'; else *slash = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    if (fsync(fd) != 0 && errno != EINVAL)
        logw("fsync(%s): %s", dir, strerror(errno));
    close(fd);
}

/* Entries with no signal aren't persisted; pure-background apps are kept while they carry restart_count (BOUNCE_SUPPRESS_RC), avg_swap_kb or a confirmed category. */
static bool score_persistable(const AppScore *s) {
    return !(s->fg_count == 0 && s->restart_count == 0 &&
             s->avg_swap_kb <= 0 && !s->category_confirmed);
}

/* One v9 line: name fg_count last_fg restart_count avg_swap_kb
 * session_count category confirmed dc_kill_count adaptive_kill_count
 * first_seen avg_session_duration_s category_confirmed_at.
 * Older readers accept shorter lines, so fields are append-only. */
static int score_write_entry(FILE *f, const AppScore *s, int c) {
    return fprintf(f, "%s %d %ld %d %ld %d %d %d %d %d %ld %ld %ld\n",
                   s->name, s->fg_count, (long)s->raw_last_fg_do_not_read_directly,
                   s->restart_count, s->avg_swap_kb,
                   s->session_count, c, s->category_confirmed ? 1 : 0,
                   s->dc_kill_count, s->adaptive_kill_count,
                   (long)s->first_seen, s->avg_session_duration_s,
                   (long)s->category_confirmed_at);
}

/* Replace the journal with an empty one tagged with `seq` (tmp + rename). */
static bool score_journal_reset(unsigned long seq) {
    char tmp[sizeof(SCORE_JOURNAL_FILE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", SCORE_JOURNAL_FILE);
    FILE *jf = fopen(tmp, "w");
    if (!jf) return false;
    bool ok = fprintf(jf, "# journal seq=%lu\n", seq) >= 0;
    if (!stdio_commit(jf)) ok = false;
    if (ok && rename(tmp, SCORE_JOURNAL_FILE) != 0) ok = false;
    if (!ok) { unlink(tmp); return false; }
    fsync_parent_dir(SCORE_JOURNAL_FILE);
    return true;
}

/* Full rewrite of SCORE_FILE, then a fresh journal; nothing is marked clean until the rename succeeds, and on failure every entry stays dirty. */
static bool score_save_full(void) {
    char tmp_path[sizeof(SCORE_FILE) + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", SCORE_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) { loge("Scores: cannot open %s: %s", tmp_path, strerror(errno)); return false; }

    unsigned long new_seq = g_score_seq + 1;
    bool ok = fprintf(f, "# lmk usage scores v9 gen=%d seq=%lu\n",
                      LEARN_RESET_GEN, new_seq) >= 0;

    static const char * const SEC_TAG[]  = { "NATIVE", "SYSAPP", "USER" };
    static const char * const SEC_DESC[] = {
        "native daemons  (kernel / HAL / bionic)",
        "system app processes  (com.android.* / lineageos / OEM)",
        "user-installed apps"
    };

    int saved = 0;
    for (int cat = 0; cat < 3; cat++) {
        bool wrote_hdr = false;
        for (int i = 0; i < g_score_count; i++) {
            const AppScore *s = &g_scores[i];
            if (!score_persistable(s)) continue;
            int c = s->category;
            if (c < 0 || c > 2) c = classify_score_cat(s->name);
            if (c != cat) continue;
            if (!wrote_hdr) {
                if (fprintf(f, "#\n# [%s] %s\n", SEC_TAG[cat], SEC_DESC[cat]) < 0) ok = false;
                wrote_hdr = true;
            }
            if (score_write_entry(f, s, c) < 0) ok = false;
            saved++;
        }
    }
    if (!stdio_commit(f)) ok = false;
    if (!ok) {
        loge("Scores: write to %s failed, keeping previous save", tmp_path);
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, SCORE_FILE) != 0) {
        loge("Scores: rename %s -> %s failed (%s)", tmp_path, SCORE_FILE, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    fsync_parent_dir(SCORE_FILE);

    g_score_seq = new_seq;
    for (int i = 0; i < g_score_count; i++)
        if (score_persistable(&g_scores[i])) g_scores[i].dirty = false;
    g_scores_dirty = false;

    if (score_journal_reset(g_score_seq)) {
        g_score_journal_pending = 0;
        g_score_journal_ok = true;
    } else {
        /* The old journal carries a lower seq and is ignored on load. */
        g_score_journal_ok = false;
        loge("Scores: journal reset failed (%s), next save rewrites it", strerror(errno));
    }
    logi("Scores: full save %d entries (v9, seq=%lu)", saved, g_score_seq);
    return true;
}

/* Append only changed entries to the journal instead of rewriting the table; entries turn clean only after the flush.
 * A failed append may leave a partial line, so the journal is distrusted and the next save is a full rewrite. */
static void score_save_incremental(void) {
    int to_write = 0;
    for (int i = 0; i < g_score_count; i++)
        if (g_scores[i].dirty) to_write++;

    if (to_write == 0) { g_scores_dirty = false; return; }
    if (!g_score_journal_ok) { score_save_full(); return; }

    FILE *f = fopen(SCORE_JOURNAL_FILE, "a");
    if (!f) { loge("Scores: cannot open journal: %s", strerror(errno)); return; }

    bool ok = true;
    for (int i = 0; i < g_score_count; i++) {
        const AppScore *s = &g_scores[i];
        if (!s->dirty) continue;
        int c = s->category;
        if (c < 0 || c > 2) c = classify_score_cat(s->name);
        if (score_write_entry(f, s, c) < 0) ok = false;
    }
    if (!stdio_commit(f)) ok = false;
    if (!ok) {
        loge("Scores: journal write failed (%s), entries stay dirty", strerror(errno));
        g_score_journal_ok = false;
        return;
    }
    for (int i = 0; i < g_score_count; i++) g_scores[i].dirty = false;
    g_score_journal_pending += to_write;
    g_scores_dirty = false;
    logi("Scores: journal +%d entr%s (%d pending consolidation)",
         to_write, to_write == 1 ? "y" : "ies", g_score_journal_pending);

    if (g_score_journal_pending >= SCORE_JOURNAL_CONSOLIDATE_N) {
        logi("Scores: journal threshold reached, consolidating");
        score_save_full();
    }
}

static void score_save(void) {
    if (!g_scores_dirty) return;
    score_save_incremental();
}

static void score_compact(void) {
    time_t now = time(NULL);
    if (now - g_score_last_compact < SCORE_COMPACT_INTVL_S) return;
    g_score_last_compact = now;

    int kept = 0, pruned = 0;
    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        if (s->raw_last_fg_do_not_read_directly != 0 && (now - s->raw_last_fg_do_not_read_directly) > SCORE_STALE_PRUNE_S) {
            /* Same leak as slot eviction: restore any live cgroup limit before the pruned entry is dropped. */
            score_evict_release_cgroups(s);
            pruned++;
            continue;
        }
        if (kept != i) g_scores[kept] = *s;
        kept++;
    }
    if (pruned > 0) {
        g_score_count  = kept;
        g_score_hash_dirty = true;
        logi("Scores: compacted, pruned %d stale entr%s (%d remain)",
             pruned, pruned == 1 ? "y" : "ies", kept);

        score_save_full();
    }
}

static void score_load(bool read_only) {
    FILE *f = fopen(SCORE_FILE,"r");
    if (!f) { if (!read_only) logi("Scores: no data file – starting fresh"); return; }
    char line[512]; int loaded = 0;

    bool gen_checked = false, need_reset = false;
    while (fgets(line, sizeof(line), f) && g_score_count < SCORE_MAX_APPS) {
        if (line[0] == '#') {
            if (!gen_checked) {
                int file_gen = 0;

                unsigned long file_seq = 0;
                sscanf(line, "# lmk usage scores v%*d gen=%d seq=%lu", &file_gen, &file_seq);
                g_score_seq = file_seq;   /* 0 for files written before seq existed */
                gen_checked = true;
                need_reset = (file_gen != LEARN_RESET_GEN);
                if (need_reset && !read_only) {
                    char archived[300];
                    snprintf(archived, sizeof(archived), "%s.gen%d",
                             LEARN_LOG_FILE, file_gen);
                    if (rename(LEARN_LOG_FILE, archived) == 0)
                        logw("LearnReset: archived %s -> %s (gen %d -> %d)",
                             LEARN_LOG_FILE, archived, file_gen, LEARN_RESET_GEN);
                    if (unlink(RANK_CACHE_FILE) == 0)
                        logw("LearnReset: cleared %s", RANK_CACHE_FILE);
                    logw("LearnReset: category_confirmed will be cleared for "
                         "re-verification; usage counters (fg_count/"
                         "restart_count/session_count/avg_swap_kb) preserved");
                }
            }
            continue;
        }
        if (line[0] == '\n') continue;
        char pkg[256]; int cnt; long ts; int rc; long avg_sw; int ses; int cat;
        int confirmed_field = 0, dckills = 0, adpkills = 0;
        long first_seen_ts = 0;
        long avg_session_dur = 0;
        long confirmed_at_ts = 0;
        int fields = sscanf(line, "%255s %d %ld %d %ld %d %d %d %d %d %ld %ld %ld",
                            pkg, &cnt, &ts, &rc, &avg_sw, &ses, &cat,
                            &confirmed_field, &dckills, &adpkills,
                            &first_seen_ts, &avg_session_dur, &confirmed_at_ts);
        if (fields < 3) continue;
        if (cnt < 0) cnt = 0;   /* defensive: %d can parse a negative */
        if (fields < 5) { rc = 0; avg_sw = 0; }
        if (fields < 6) { ses = 0; }
        if (fields < 7) { cat = classify_score_cat(pkg); }
        /* v6 added a "confirmed" field; a 9-field legacy line has its dckills/adpkills shifted back into place per field count. */
        bool has_confirmed_field = (fields >= 10) && !need_reset;
        if (fields == 9) {
            adpkills = dckills;
            dckills  = confirmed_field;
            confirmed_field = 0;
        } else if (fields == 8) {
            dckills = confirmed_field;
            confirmed_field = 0;
        }
        /* fg_count == 0 is legal (v9+): background-only apps keep bounce history, swap EMA and confirmed category; entries with none of those are dropped. */
        if (cnt == 0 && rc <= 0 && avg_sw <= 0 &&
            !(has_confirmed_field && confirmed_field == 1))
            continue;
        /* v7 added first_seen and v8 avg_session_duration_s; shorter lines leave them at the 0 "unknown" sentinel since sscanf() skips unreached args. */
        if (cat < 0 || cat > 2) cat = classify_score_cat(pkg);
        AppScore *s = &g_scores[g_score_count++];
        strncpy(s->name, pkg, 255); s->name[255] = '\0';
        s->fg_count          = cnt < FG_COUNT_MAX ? cnt : FG_COUNT_MAX;
        s->raw_last_fg_do_not_read_directly           = (time_t)ts;
        s->restart_count     = rc;
        s->avg_swap_kb       = avg_sw;
        s->session_count     = ses;
        s->category          = cat;
        s->dc_kill_count     = dckills;
        s->adaptive_kill_count = adpkills;
        s->first_seen        = (time_t)first_seen_ts;
        s->avg_session_duration_s = avg_session_dur;
        s->category_confirmed_at = (fields >= 13) ? (time_t)confirmed_at_ts : 0;
        s->session_start_t   = 0; /* in-memory only, re-seeds on next fg touch */
        s->static_class_cache = -1;
        if (has_confirmed_field && confirmed_field == 1) {
            /* v6: category was already dumpsys-confirmed in a prior run -
             * trust it as-is, no re-derivation or dumpsys re-queue needed. */
            s->category_confirmed = true;
        } else {
            /* Legacy, unconfirmed or gen-mismatched entry: use the heuristic path and queue a dumpsys check only when ambiguous. */
            int fresh_guess = classify_score_cat(pkg);
            if (fresh_guess != 2) {
                s->category = fresh_guess;
                s->category_confirmed = true;

                s->category_confirmed_at = time(NULL);
            } else {
                classify_queue_maybe(pkg);
            }
        }
        loaded++;
    }
    fclose(f);
    g_score_hash_dirty = true;
    logi("Scores: loaded %d entries", loaded);

    g_score_journal_ok = false;   /* first save after boot rewrites base + journal */
    FILE *jf = fopen(SCORE_JOURNAL_FILE, "r");
    if (!jf) return;
    int replayed = 0;
    bool journal_hdr = false;
    while (fgets(line, sizeof(line), jf) && g_score_count < SCORE_MAX_APPS) {
        if (line[0] == '#') {
            unsigned long js;
            if (!journal_hdr && sscanf(line, "# journal seq=%lu", &js) == 1) {
                journal_hdr = true;
                if (js != g_score_seq) {
                    logw("Scores: ignoring stale journal (seq %lu, base seq %lu)",
                         js, g_score_seq);
                    break;
                }
            }
            continue;
        }
        if (line[0] == '\n') continue;
        /* A journal without a header predates seq and is valid only against a base that predates it too. */
        if (!journal_hdr && g_score_seq != 0) {
            logw("Scores: ignoring unlabeled journal (base seq %lu)", g_score_seq);
            break;
        }
        /* A torn last line (crash mid-append) must not parse as a shorter, wrong record. */
        size_t ll = strlen(line);
        if (line[ll - 1] != '\n') continue;
        char pkg[256]; int cnt; long ts; int rc; long avg_sw; int ses; int cat;
        int confirmed_field = 0, dckills = 0, adpkills = 0;
        long first_seen_ts = 0;
        long avg_session_dur = 0;
        long confirmed_at_ts = 0;
        int fields = sscanf(line, "%255s %d %ld %d %ld %d %d %d %d %d %ld %ld %ld",
                            pkg, &cnt, &ts, &rc, &avg_sw, &ses, &cat,
                            &confirmed_field, &dckills, &adpkills,
                            &first_seen_ts, &avg_session_dur, &confirmed_at_ts);
        if (fields < 3) continue;
        if (cnt < 0) cnt = 0;   /* defensive: %d can parse a negative */
        if (fields < 5) { rc = 0; avg_sw = 0; }
        if (fields < 6) { ses = 0; }
        if (fields < 7) { cat = classify_score_cat(pkg); }
        bool has_confirmed_field = (fields >= 10) && !need_reset;
        if (fields == 9) {
            adpkills = dckills;
            dckills  = confirmed_field;
            confirmed_field = 0;
        } else if (fields == 8) {
            dckills = confirmed_field;
            confirmed_field = 0;
        }
        /* Same background-only rule as the base loop: cnt == 0 journal lines are valid with bounce history, a swap EMA or a trusted confirmed category. */
        if (cnt == 0 && rc <= 0 && avg_sw <= 0 &&
            !(has_confirmed_field && confirmed_field == 1))
            continue;
        if (cat < 0 || cat > 2) cat = classify_score_cat(pkg);

        AppScore *s = score_lookup(pkg);
        bool is_new = !s;
        if (!s) s = &g_scores[g_score_count++];
        strncpy(s->name, pkg, 255); s->name[255] = '\0';
        s->fg_count            = cnt < FG_COUNT_MAX ? cnt : FG_COUNT_MAX;
        s->raw_last_fg_do_not_read_directly             = (time_t)ts;
        s->restart_count       = rc;
        s->avg_swap_kb         = avg_sw;
        s->session_count       = ses;
        s->category            = cat;
        s->dc_kill_count       = dckills;
        s->adaptive_kill_count = adpkills;

        if (fields >= 11) s->first_seen = (time_t)first_seen_ts;
        /* v8: overwrite only if present, so a pre-v8 journal line doesn't reset a real avg_session_duration_s to the legacy 0. */
        if (fields >= 12) s->avg_session_duration_s = avg_session_dur;
        /* v9: overwrite only if present, same as first_seen/avg_session_duration_s. */
        if (fields >= 13) s->category_confirmed_at = (time_t)confirmed_at_ts;
        if (is_new) { s->session_start_t = 0; s->static_class_cache = -1; g_score_hash_dirty = true; }
        if (has_confirmed_field && confirmed_field == 1) {
            s->category_confirmed = true;
        } else if (!s->category_confirmed) {
            int fresh_guess = classify_score_cat(pkg);
            if (fresh_guess != 2) {
                s->category = fresh_guess;
                s->category_confirmed = true;

                s->category_confirmed_at = time(NULL);
            } else {
                classify_queue_maybe(pkg);
            }
        }
        replayed++;
    }
    fclose(jf);
    if (replayed > 0) logi("Scores: replayed %d journal entr%s",
                           replayed, replayed == 1 ? "y" : "ies");
}

static void score_maybe_save(void) {
    time_t now = time(NULL);
    if (now - g_score_last_save >= SCORE_SAVE_INTVL) {
        score_compact();
        score_save(); g_score_last_save = now;
    }
}

/* Rank cache: per-app swap + rank snapshot (name avg_swap_kb rank), so cmp_killable_zram sorts correctly right after a restart. */

typedef struct { char name[256]; long swap; int rank; } RankCacheEntry;
static int rank_cache_cmp(const void *a, const void *b) {
    return cmp3(((const RankCacheEntry *)a)->rank, ((const RankCacheEntry *)b)->rank);
}

static void rank_cache_save(ProcInfo *tbl, int cnt) {
    static RankCacheEntry entries[SCORE_MAX_APPS];
    int n = 0;
    for (int i = 0; i < cnt && n < SCORE_MAX_APPS; i++) {
        ProcInfo *p = &tbl[i];
        if (!p->name[0]) continue;

        AppScore *as = score_lookup(p->name);
        int cat = (as && as->category_confirmed)
                      ? as->category : classify_score_cat(p->name);

        if (cat == 0) continue; /* native binaries only */
        if (is_rank_exempt(p->name)) continue;
        /* Use live swap_kb blended with stored avg */
        long swap = p->swap_kb;
        if (as && as->avg_swap_kb > 0)
            swap = (as->avg_swap_kb * 80 + p->swap_kb * 20) / 100;
        strncpy(entries[n].name, p->name, sizeof(entries[n].name) - 1);
        entries[n].name[sizeof(entries[n].name) - 1] = '\0';
        entries[n].swap = swap;
        entries[n].rank = p->rank;
        n++;
    }
    qsort(entries, (size_t)n, sizeof(RankCacheEntry), rank_cache_cmp);

    char tmp_path[sizeof(RANK_CACHE_FILE) + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", RANK_CACHE_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    /* n= lets the loader reject a file that lost lines. */
    bool ok = fprintf(f, "# lmk rank cache v2 ts=%ld n=%d\n", (long)time(NULL), n) >= 0;
    for (int i = 0; i < n; i++)
        if (fprintf(f, "%s %ld %d\n", entries[i].name, entries[i].swap, entries[i].rank) < 0)
            ok = false;
    if (!stdio_commit(f)) ok = false;
    if (!ok) {
        loge("RankCache: write failed (%s), keeping previous cache", strerror(errno));
        unlink(tmp_path);
        return;
    }
    if (rename(tmp_path, RANK_CACHE_FILE) != 0) {
        loge("RankCache: rename %s -> %s failed (%s)", tmp_path, RANK_CACHE_FILE, strerror(errno));
        unlink(tmp_path);
        return;
    }
    fsync_parent_dir(RANK_CACHE_FILE);
}

static void rank_cache_load(void) {
    FILE *f = fopen(RANK_CACHE_FILE, "r");
    if (!f) return;
    char line[300]; int loaded = 0;

    /* Validation pass: a v2 header promises n records; a short file is rejected whole, v1 files (no n=) load as before. */
    int expect = -1, valid = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            int hn;
            const char *np = strstr(line, " n=");
            if (expect < 0 && np && sscanf(np, " n=%d", &hn) == 1 && hn >= 0) expect = hn;
            continue;
        }
        char pkg_v[256]; long sw_v; int rk_v;
        if (sscanf(line, "%255s %ld %d", pkg_v, &sw_v, &rk_v) >= 2) valid++;
    }
    if (ferror(f) || (expect >= 0 && valid != expect)) {
        logw("RankCache: rejecting corrupt cache (%d of %d records)", valid, expect);
        fclose(f);
        return;
    }
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pkg[256]; long avg_sw; int rank;
        if (sscanf(line, "%255s %ld %d", pkg, &avg_sw, &rank) < 2) continue;

        (void)rank;
        AppScore *s = score_lookup(pkg);
        if (!s) {
            s = score_alloc_slot();
            memset(s, 0, sizeof(*s));
            strncpy(s->name, pkg, 255); s->name[255] = '\0';
            s->category = classify_score_cat(pkg);

            s->first_seen = time(NULL);
            if (s->category != 2) {
                s->category_confirmed = true;
                s->category_confirmed_at = time(NULL);
            } else if (!s->category_confirmed) {
                classify_queue_maybe(pkg);
            }
        }
        /* Only overwrite avg_swap_kb if the score entry has none */
        if (s->avg_swap_kb == 0 && avg_sw > 0)
            s->avg_swap_kb = avg_sw;
        loaded++;
    }
    fclose(f);
    if (loaded > 0) logi("RankCache: loaded %d entries (avg_swap warm-up)", loaded);
}

static void rank_cache_maybe_save(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    if (now - g_rank_cache_last_save >= RANK_CACHE_SAVE_S) {
        rank_cache_save(tbl, cnt);
        g_rank_cache_last_save = now;
    }
}

/* Process classification */
static Priority classify(const ProcInfo *p) {

    AppScore *cs = score_lookup(p->name);
    int cached = cs ? cs->static_class_cache : -1;
    if (cached == -1) {
        cached = (name_matches(p->name, NEVER_KILL) ||
                  name_matches(p->name, SERVICE_EXEMPT) ||
                  is_launcher_like(p->name) ||
                  is_ime_like(p->name) ||
                  is_active_widget_provider(p->name)) ? 1 : 0;
        if (cs) cs->static_class_cache = cached;
    }
    if (cached == 1)                          return PRIO_NEVER;
    if (p->oom_adj <= 0)                      return PRIO_NEVER;
    if (p->oom_adj <= ADJ_VISIBLE_MAX)        return PRIO_SEMI_PROTECTED;
    if (is_media_player(p->name))             return PRIO_SEMI_PROTECTED;
    if (p->oom_adj <= ADJ_SERVICE_MAX)        return PRIO_BACKGROUND;
    if (p->oom_adj <= ADJ_CACHED_MAX)         return PRIO_CACHED;
    return PRIO_JUNK;
}

/* Defined later; enumerate_procs() needs them for a clean true-focus/recency signal instead of raw oom_adj. */
static bool is_fg_trackable(const char *n);
static bool is_genuine_fg(const char *n, long oom_adj);
static AppScore *score_lookup(const char *name);

static bool is_structurally_headless(const AppScore *s, time_t now) {
    return s && s->fg_count == 0
        && s->adj0_blip_total_s >= BLIP_HEADLESS_MIN_S
        && s->first_seen > 0
        && (now - s->first_seen) >= RANK_COLD_S;
}

static time_t effective_fg_recency(const ProcInfo *p) {
    if (p->last_genuine_fg) return p->last_genuine_fg;
    if (!is_fg_trackable(p->name)) return 0;
    AppScore *as = score_lookup(p->name);
    if (is_structurally_headless(as, time(NULL))) return 0;
    return p->raw_last_fg_do_not_read_directly;
}

/* Process enumeration */

static AppScore *lookup_or_create_score_stub(ProcInfo *p) {
    AppScore *as     = score_lookup(p->name);
    if (!as && !is_rank_exempt(p->name)) {
        as = score_alloc_slot();
        memset(as, 0, sizeof(*as));
        strncpy(as->name, p->name, 255); as->name[255] = '\0';
        as->category = classify_score_cat(p->name);
        as->static_class_cache = -1;

        as->first_seen = time(NULL);
        as->dirty = true;
        g_scores_dirty = true;
        g_score_hash_dirty = true;

        if (as->category != 2) {
            as->category_confirmed = true;
            as->category_confirmed_at = time(NULL);
        }
        else classify_queue_maybe(p->name);
    }
    return as;
}

static const char *rank_name(int r) {
    switch (r) {
        case RANK_EXEMPT: return "EXEMPT";
        case RANK_TIER1:  return "TIER1";
        case RANK_TIER2:  return "TIER2";
        case RANK_TIER3:  return "TIER3";
        case RANK_TIER4:  return "BACKGROUND";
        default:          return "?";
    }
}

/* Rank for one process: exempt short-circuit, true_fg, app_rank(), the RANK_BACKGROUND override, then transition logging and FlapWatch. */
static void compute_proc_rank(ProcInfo *p, AppScore *as) {

        if (is_rank_exempt(p->name))
            p->rank = RANK_EXEMPT;
        else {

            p->true_fg = is_genuine_fg(p->name, p->oom_adj);

            if (p->true_fg && as) as->last_true_fg_seen = time(NULL);

            if (as && as->rank_computed_seq == g_rank_seq) {
                p->rank = as->rank_computed_result;
                return;
            }
            if (as) as->rank_computed_seq = g_rank_seq;

            bool true_fg_for_rank = p->true_fg;
            if (as) {
                time_t rank_now = time(NULL);
                if (p->true_fg) {
                    if (as->rank_fg_hold_start == 0)
                        as->rank_fg_hold_start = rank_now;
                    true_fg_for_rank =
                        (rank_now - as->rank_fg_hold_start) >= RANK1_HOLD_S;
                } else {
                    as->rank_fg_hold_start = 0;
                }
            }
            time_t rank_recency = effective_fg_recency(p);
            p->rank = app_rank(as, true_fg_for_rank,
                               rank_recency,
                               p->last_genuine_fg != 0,
                               as ? as->session_count : 0);

            bool bg_structural = false, bg_bounce = false, bg_decayed = false;
            bool bg_lowfreq = false;
            if (!p->true_fg) {
                int cat = (as && as->category_confirmed)
                              ? as->category : classify_score_cat(p->name);

                time_t recency = effective_fg_recency(p);

                time_t last_seen = (as && as->last_true_fg_seen > recency)
                                    ? as->last_true_fg_seen : recency;
                bool idle_enough = (last_seen == 0) ||
                                    ((time(NULL) - last_seen) >= RANK_COLD_S);

                bg_structural = (cat == 1) && idle_enough;

                if (!idle_enough) {
                    if (as) as->bg_idle_since = 0;
                } else if (as && as->bg_idle_since == 0) {
                    as->bg_idle_since = time(NULL);
                }
                bool idle_confirmed = idle_enough && as && as->bg_idle_since &&
                    (time(NULL) - as->bg_idle_since) >= BG_IDLE_DEBOUNCE_S;
                if (idle_confirmed) {

                    if (as && as->restart_count >= BOUNCE_SUPPRESS_RC &&
                        as->last_restart_t &&
                        (time(NULL) - as->last_restart_t) >= BOUNCE_DECAY_S) {
                        as->restart_count = 0;
                        as->dirty = true;
                        bg_decayed = true;
                    }
                    bg_bounce = as && as->restart_count >= BOUNCE_SUPPRESS_RC;

                    if (!bg_structural && !bg_bounce &&
                        as && as->first_seen) {
                        time_t tracked_s = time(NULL) - as->first_seen;
                        if (tracked_s >= LOWFREQ_MIN_OBS_S) {
                            double fg_rate = (double)as->fg_count /
                                              ((double)tracked_s / 3600.0);
                            bg_lowfreq = (fg_rate < LOWFREQ_FG_RATE_THRESH);
                        }
                    }
                }
                if (bg_structural || bg_bounce || bg_lowfreq)
                    p->rank = RANK_TIER4;
            }

            if (as && p->rank != as->last_logged_rank) {
                if (p->rank == RANK_TIER4)
                    logi("Rank: [%s] -> BACKGROUND (%s)", p->name,
                         bg_structural ? "structural" :
                         bg_bounce ? "bounce-proven" : "low-freq");
                else if (as->last_logged_rank == RANK_TIER4) {
                    if (bg_decayed)
                        logi("Rank: [%s] -> normal (bounce evidence expired)",
                             p->name);
                    else
                        logi("Rank: [%s] -> normal (rank=%d, genuinely reopened)",
                             p->name, p->rank);
                }
                else {

                    logi("Rank: [%s] %s -> %s (recency_age=%lds rc=%d true_fg=%d)",
                         p->name, rank_name(as->last_logged_rank), rank_name(p->rank),
                         rank_recency ? (long)(time(NULL) - rank_recency) : -1L,
                         as->restart_count, p->true_fg ? 1 : 0);
                }
                as->last_logged_rank = p->rank;
                as->dirty = true;

                time_t fnow = time(NULL);
                if (as->flap_window_start == 0)
                    as->flap_window_start = fnow;
                as->flap_count++;
                time_t win_elapsed = fnow - as->flap_window_start;
                if (win_elapsed >= FLAP_WINDOW_MIN_S) {
                    double rate = (double)as->flap_count /
                                  ((double)win_elapsed / 3600.0);
                    if (rate >= FLAP_RATE_WARN_THRESH && !as->flap_warned) {
                        logw("FlapWatch: [%s] flapping BACKGROUND<->normal "
                             "at %.1f/hr (%d in %ldm) — candidate for "
                             "is_fg_trackable() exclusion review", p->name,
                             rate, as->flap_count, (long)(win_elapsed / 60));
                        as->flap_warned = true;
                    }
                }
                if (win_elapsed >= FLAP_WINDOW_RESET_S) {
                    as->flap_count = 0;
                    as->flap_window_start = fnow;
                    as->flap_warned = false;
                }
            }

            if (as) as->rank_computed_result = p->rank;
        }
}
static void check_proc_runaway(ProcInfo *p) {

        if (p->prio != PRIO_NEVER && p->rank != RANK_EXEMPT) {
            check_runaway_growth(p->name, time(NULL), p->rss_kb);
            AppScore *gs = score_lookup(p->name);
            if (gs && gs->flagged_runaway && p->oom_adj != ADJ_FOREGROUND)
                p->prio = PRIO_JUNK;
        }
}
static void track_proc_bounce_window(ProcInfo *p, time_t now) {
    p->bounce_count  = 0;
    p->bounce_first  = 0;
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (g_kill_hist[i].name[0] &&
            strcmp(g_kill_hist[i].name, p->name) == 0 &&
            (now - g_kill_hist[i].killed_at) < BOUNCE_WINDOW_S) {
            if (p->bounce_count == 0) p->bounce_first = g_kill_hist[i].killed_at;
            p->bounce_count++;
        }
    }
}
static void enforce_tier_caps(ProcInfo *tbl, int cnt) {

    for (int tier = RANK_TIER1; tier <= RANK_TIER2; tier++) {
        int cap = (tier == RANK_TIER1) ? TIER1_MAX_SLOTS : TIER2_MAX_SLOTS;
        int demote_to = tier + 1;

        char uniq_name[64][256];
        int  uniq_score[64];
        bool uniq_demoted[64] = {0};
        int  uniq_n = 0;
        for (int i = 0; i < cnt; i++) {
            if (tbl[i].rank != tier || tbl[i].true_fg) continue;
            int found = -1;
            for (int u = 0; u < uniq_n; u++)
                if (strcmp(uniq_name[u], tbl[i].name) == 0) { found = u; break; }
            if (found >= 0) {
                if (tbl[i].score < uniq_score[found]) uniq_score[found] = tbl[i].score;
            } else if (uniq_n < 64) {
                strncpy(uniq_name[uniq_n], tbl[i].name, sizeof(uniq_name[0]) - 1);
                uniq_name[uniq_n][sizeof(uniq_name[0]) - 1] = '\0';
                uniq_score[uniq_n] = tbl[i].score;
                uniq_n++;
            }
        }
        int excess = uniq_n - cap;
        while (excess > 0) {
            int weakest = -1;
            for (int u = 0; u < uniq_n; u++) {
                if (uniq_demoted[u]) continue;
                if (weakest == -1 || uniq_score[u] < uniq_score[weakest] ||
                    (uniq_score[u] == uniq_score[weakest] &&
                     strcmp(uniq_name[u], uniq_name[weakest]) > 0))
                    weakest = u;
            }
            if (weakest == -1) break;
            logi("TierCap: [%s] demoted TIER%d -> TIER%d (capacity %d/%d, score=%d)",
                 uniq_name[weakest], tier, demote_to, uniq_n, cap, uniq_score[weakest]);
            /* Apply to every live process of this package this tick, not just one PID, so siblings don't keep a stale rank until the next dedup resync. */
            for (int i = 0; i < cnt; i++)
                if (tbl[i].rank == tier && !tbl[i].true_fg &&
                    strcmp(tbl[i].name, uniq_name[weakest]) == 0)
                    tbl[i].rank = demote_to;
            AppScore *ws = score_lookup(uniq_name[weakest]);
            if (ws) {
                ws->rank_tier = demote_to; ws->tier_entered_t = time(NULL);

                ws->capacity_demote_t = time(NULL);
                ws->capacity_demote_from = tier;
            }
            uniq_demoted[weakest] = true;
            excess--;
        }
    }
}
static int enumerate_procs(ProcInfo *tbl, int maxn) {
    g_rank_seq++;
    DIR *dir = opendir("/proc"); if (!dir) return -1;
    int cnt = 0; struct dirent *de;
    while ((de = readdir(dir)) && cnt < maxn) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atol(de->d_name); if (pid <= 1) continue;
        ProcInfo *p = &tbl[cnt]; memset(p, 0, sizeof(*p)); p->pid = pid;
        /* Bracket every /proc/<pid> read with a starttime sample before and after; a mismatch (recycled PID) or 0 (gone) drops the entry so no snapshot mixes two processes.
         * Global-state side effects run only after the snapshot is verified. */
        unsigned long long st_before = read_proc_starttime(pid);
        if (st_before == 0) continue;
        if (proc_cmdline(pid, p->name, sizeof(p->name)) < 0) continue;
        if (!p->name[0]) continue;
        p->oom_adj       = proc_oom_adj(pid);
        proc_mem_stats(pid, &p->rss_kb, &p->swap_kb);
        if (read_proc_starttime(pid) != st_before) continue;  /* recycled / exited */
        p->starttime     = st_before;   /* identity used by kill_proc_verified() */
        p->prio          = classify(p);
        p->raw_last_fg_do_not_read_directly       = get_last_fg(p->name);
        check_bounced(p->name);

        AppScore *as = lookup_or_create_score_stub(p);

        /* Fall back to the persisted raw_last_fg_do_not_read_directly if fg_history has no entry (daemon restart; keeps recents-dismiss and retention working). */
        if (p->raw_last_fg_do_not_read_directly == 0 && as) p->raw_last_fg_do_not_read_directly = as->raw_last_fg_do_not_read_directly;
        p->score         = as ? score_compute(as) : 0;
        p->restart_count = as ? as->restart_count : 0;
        p->avg_swap_kb   = as ? as->avg_swap_kb : 0;
        p->last_genuine_fg   = as ? as->last_genuine_fg : 0;

        compute_proc_rank(p, as);
        check_proc_runaway(p);

        time_t now = time(NULL);
        track_proc_bounce_window(p, now);
        cnt++;
    }
    closedir(dir);

    enforce_tier_caps(tbl, cnt);

    return cnt;
}

/* Foreground tracking + bounce detection */

/* Name-pattern exclusion shared by is_genuine_fg() and the retention recency fallback: matching apps can never earn last_genuine_fg,
 * which decides whether falling back to raw_last_fg_do_not_read_directly is safe (see oom_pin_retained()). */
static bool is_fg_trackable(const char *n) {
    if (n[0] == '/' || strstr(n, ".persistent") ||
        strstr(n, ":persistent") || strstr(n, ".providers.") ||
        strncmp(n, "android.process.", 16) == 0)
        return false;

    if (strstr(n, "permissioncontroller") ||
        strstr(n, "cellbroadcastreceiver") ||
        strstr(n, "settings.intelligence"))
        return false;

    if (strstr(n, "apps.turbo") || strstr(n, "adservices.api"))
        return false;

    if (strstr(n, "chromecast.app"))
        return false;
    return true;
}

static bool is_genuine_fg(const char *n, long oom_adj) {

    return oom_adj == ADJ_FOREGROUND && is_fg_trackable(n) &&
           g_true_fg_name[0] != '\0' && !strcmp(n, g_true_fg_name);
}

static bool cgroup_path_for_pid(pid_t pid, char *out, size_t out_sz);
static bool cgroup_write(pid_t pid, const char *file, const char *val);

static void track_foreground(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    for (int i = 0; i < cnt; i++) {
        const char *n = tbl[i].name;
        bool genuine_fg = is_genuine_fg(n, tbl[i].oom_adj);

        AppScore *s = score_lookup(n);
        if (s) {
            if (s->was_true_fg && !genuine_fg) s->last_bg = now;

            if (genuine_fg) {
                s->fg_hold_last_genuine_t = now;
                /* The true foreground app releases any memory.high squeeze immediately; a constrained foreground app is visible jank. */
                if (s->cg_high_set) {
                    bool released = false, found_proc = false;
                    for (int j = 0; j < cnt; j++) {
                        if (strcmp(tbl[j].name, n) != 0) continue;
                        found_proc = true;
                        released = cgroup_write(tbl[j].pid, "memory.high", "max");
                        break;
                    }
                    if (released || !found_proc) {
                        s->cg_high_set = false;
                        s->cg_high_set_t = 0;
                        s->cg_high_effective = false;
                        logi("SqueezeRelease: [%s] returned to foreground — "
                             "memory.high cleared%s", n,
                             found_proc ? "" : " (process gone)");
                    } else {
                        logw("SqueezeRelease: [%s] foreground write failed — "
                             "will retry next pass", n);
                    }
                }
            } else if (s->fg_hold_start != 0 &&
                       (s->fg_hold_last_genuine_t == 0 ||
                        (now - s->fg_hold_last_genuine_t) > FG_HOLD_WOBBLE_TOLERANCE_S)) {

                logi("HoldReset: [%s] fg_hold reset after held_s=%ld (wobble_tol=%ds)",
                     n, (long)(now - s->fg_hold_start), FG_HOLD_WOBBLE_TOLERANCE_S);
                s->fg_hold_start = 0;
            }
            s->was_true_fg = genuine_fg;
        }

        if (tbl[i].oom_adj != ADJ_FOREGROUND) {
            /* Background-visible (perceptible/visible svc, 1-200): not user-foregrounded (foreground service, sync, media, notification), so recency-only touch. */
            if (tbl[i].oom_adj <= ADJ_VISIBLE_MAX) {
                update_fg_history(n, now);
                score_touch_bg(n, now, tbl[i].swap_kb);
            }
            continue;
        }
        if (!genuine_fg) {
            update_fg_history(n, now);
            score_touch_bg(n, now, tbl[i].swap_kb);

            AppScore *bs = score_lookup_or_create(n);
            time_t d = (bs->adj0_last_tick && now > bs->adj0_last_tick)
                       ? now - bs->adj0_last_tick : 0;
            if (d > 2) d = 2;
            bs->adj0_blip_total_s += d;
            bs->adj0_last_tick = now;
            continue;
        }

        AppScore *rs = score_lookup(n);
        if (rs && rs->cg_high_set &&
            cgroup_write(tbl[i].pid, "memory.high", "max")) {
            rs->cg_high_set = false;
            rs->cg_high_set_t = 0;
            logi("SqueezeRelease: [%s] memory.high cleared on genuine fg", n);
        }

        AppScore *hs = score_lookup_or_create(n);
        if (hs->fg_hold_start == 0) hs->fg_hold_start = now;
        if ((now - hs->fg_hold_start) < FG_MIN_HOLD_S) {
            update_fg_history(n, now);
            score_touch_bg(n, now, tbl[i].swap_kb);
            continue;
        }
        update_fg_history(n, now);
        score_touch(n, now, tbl[i].swap_kb);
    }
}

/* True foreground detection */

static bool confirm_true_foreground(const char *pkg) {
    FILE *fp = popen("cmd activity top 2>/dev/null", "r");
    if (!fp) return true;
    char line[512];
    bool found_activity = false, matched = false;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "ACTIVITY") && !strstr(line, "topResumedActivity") &&
            !strstr(line, "mResumedActivity")) continue;
        found_activity = true;
        if (strstr(line, pkg)) { matched = true; break; }
    }
    pclose(fp);
    return found_activity ? matched : true; /* fail open on unsupported output */
}

static bool find_true_foreground(ProcInfo *tbl, int cnt, char *out, size_t outsz) {
    for (int i = 0; i < cnt; i++) {
        if (tbl[i].oom_adj != ADJ_FOREGROUND) continue;
        const char *n = tbl[i].name;
        if (!is_fg_trackable(n)) continue;

        strncpy(out, n, outsz - 1);
        out[outsz - 1] = '\0';
        return true;
    }
    return false;
}

/* OOM pinning: active app retention */

#define PIN_STICKY_PCT     20   /* sticky bonus = 20% of SCORE_MAX */
#define PIN_STICKY_BONUS   ((SCORE_MAX * PIN_STICKY_PCT) / 100)
static char    g_prev_pinned[RETAIN_MAX_N][256];
static int     g_prev_pinned_n = 0;
static time_t  g_kill_roi_log_t = 0;

static bool was_prev_pinned(const char *name) {
    for (int i = 0; i < g_prev_pinned_n; i++)
        if (strcmp(g_prev_pinned[i], name) == 0) return true;
    return false;
}

static void oom_pin_retained(ProcInfo *tbl, int cnt, int avail_pct) {

    bool allow_new_pins = avail_pct >= PIN_ALLOW_FLOOR_PCT;

    time_t now = time(NULL);

    bool   done[2048];
    int    max_done = cnt < 2048 ? cnt : 2048;
    memset(done, 0, (size_t)max_done * sizeof(bool));

    int  retained = 0;
    int  cg_low_ok = 0;
    char retained_names[512] = "";
    char local_pinned_name[RETAIN_MAX_N][256];
    int  local_pinned_count = 0;

#define PIN_APPLY(p) do { \
        AppScore *as = score_lookup((p)->name); \
        if (as) as->retain_pin_t = now; /* grants the 8s opportunistic-kill grace */ \
         \
        char lowbuf[32]; \
        snprintf(lowbuf, sizeof(lowbuf), "%ld", (long)(p)->rss_kb * 1024); \
        if (cgroup_write((p)->pid, "memory.low", lowbuf)) { \
            if (as) as->cg_low_set = true; \
            cg_low_ok++; \
        } \
        retained++; \
        size_t used = strlen(retained_names); \
        if (used < sizeof(retained_names) - 1) \
            snprintf(retained_names + used, sizeof(retained_names) - used, \
                     "%s%s", used ? "," : "", (p)->name); \
        if (local_pinned_count < RETAIN_MAX_N) { \
            strncpy(local_pinned_name[local_pinned_count], (p)->name, \
                    sizeof(local_pinned_name[0]) - 1); \
            local_pinned_name[local_pinned_count][sizeof(local_pinned_name[0]) - 1] = 0; \
            local_pinned_count++; \
        } \
    } while (0)

    /* Phases A and B select new pins only when RAM isn't critical (see allow_new_pins); release below always runs. */
    if (allow_new_pins) {
    /* Phase A: reserve TIER1_PIN_MAX guaranteed slots, tier1 only, so a recently-used tier1 app can't be shut out by a higher-scoring tier2 app. */
    for (int k = 0; k < TIER1_PIN_MAX; k++) {
        int best = -1;
        for (int i = 0; i < cnt; i++) {
            if (done[i]) continue;
            ProcInfo *p = &tbl[i];
            if (p->rank != RANK_TIER1) continue;
            if (p->prio <= PRIO_SEMI_PROTECTED) continue;
            if (p->score <= 0) continue;
            time_t recency = effective_fg_recency(p);
            if (recency == 0) continue;

            /* Skip apps that only just left foreground, to avoid pinning mid-transition on a single flickering tick; still eligible next tick. */
            if ((now - recency) < PIN_SETTLE_S) continue;
            int eff = p->score + (was_prev_pinned(p->name) ? PIN_STICKY_BONUS : 0);
            int best_eff = best >= 0
                ? tbl[best].score + (was_prev_pinned(tbl[best].name) ? PIN_STICKY_BONUS : 0)
                : -1;
            if (best == -1 || eff > best_eff ||
                (eff == best_eff && strcmp(p->name, tbl[best].name) < 0))
                best = i;
        }
        if (best < 0) break;
        done[best] = true;
        PIN_APPLY(&tbl[best]);
    }

    for (int k = 0; k < TIER2_PIN_MAX; k++) {
        int best = -1;
        for (int i = 0; i < cnt; i++) {
            if (done[i]) continue;
            ProcInfo *p = &tbl[i];
            if (p->rank != RANK_TIER1 && p->rank != RANK_TIER2) continue;
            if (p->prio <= PRIO_SEMI_PROTECTED) continue;
            if (p->score <= 0) continue;

            if (p->rank == RANK_TIER2 && !p->last_genuine_fg) continue;
            time_t recency = effective_fg_recency(p);
            if (recency == 0) continue;

            if (p->rank == RANK_TIER2 && (now - recency) > RETAIN_FG_AGE_S) continue;
            if ((now - recency) < PIN_SETTLE_S) continue;
            int eff = p->score + (was_prev_pinned(p->name) ? PIN_STICKY_BONUS : 0);
            int best_eff = best >= 0
                ? tbl[best].score + (was_prev_pinned(tbl[best].name) ? PIN_STICKY_BONUS : 0)
                : -1;
            if (best == -1 || eff > best_eff ||
                (eff == best_eff && strcmp(p->name, tbl[best].name) < 0))
                best = i;
        }
        if (best < 0) break;
        done[best] = true;
        PIN_APPLY(&tbl[best]);
    }
    } /* allow_new_pins */
#undef PIN_APPLY

    for (int i = 0; i < g_prev_pinned_n; i++) {
        bool still_pinned = false;
        for (int k = 0; k < local_pinned_count; k++)
            if (strcmp(g_prev_pinned[i], local_pinned_name[k]) == 0) { still_pinned = true; break; }
        if (still_pinned) continue;
        AppScore *das = score_lookup(g_prev_pinned[i]);
        if (!das || !das->cg_low_set) continue;
        bool found = false;
        for (int j = 0; j < cnt; j++) {
            if (strcmp(tbl[j].name, g_prev_pinned[i]) == 0) {
                cgroup_write(tbl[j].pid, "memory.low", "0");
                das->cg_low_set = false;
                found = true;
                break;
            }
        }

        if (!found) das->cg_low_set = false;
    }

    /* Remember this cycle's retained set (names only) for next tick's stickiness bonus. */
    g_prev_pinned_n = local_pinned_count;
    for (int i = 0; i < local_pinned_count; i++) {
        strncpy(g_prev_pinned[i], local_pinned_name[i], sizeof(g_prev_pinned[0]) - 1);
        g_prev_pinned[i][sizeof(g_prev_pinned[0]) - 1] = 0;
    }

    if (retained > 0 && now - g_retain_log_t >= LOG_RATELIMIT_S) {
        g_retain_log_t = now;
        logi("Retain: protected %d app(s) [%s] (8s opportunistic-kill grace, avail=%d%%, cg_low=%d/%d)",
             retained, retained_names, avail_pct, cg_low_ok, retained);
    }
}

/* Kill ordering */
/* RAM kill ordering: rank desc (stale first), then prio, score, swap, age, rss */

static inline int kill_rank_key(int rank) {
    return rank == RANK_TIER4 ? RANK_TIER3 : rank;
}

static int cmp_killable_ram(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    int ra = kill_rank_key(pa->rank), rb = kill_rank_key(pb->rank);
    if (ra != rb)
        return cmp3(rb, ra);                          /* rank desc */
    if (pa->prio != pb->prio)
        return cmp3((int)pa->prio, (int)pb->prio);    /* prio asc */
    if (pa->score != pb->score)
        return cmp3(pa->score, pb->score);            /* score asc */
    if (pa->avg_swap_kb != pb->avg_swap_kb)
        return cmp3(pb->avg_swap_kb, pa->avg_swap_kb);/* avg swap desc */
    if (pa->restart_count != pb->restart_count)
        return cmp3(pb->restart_count, pa->restart_count); /* restarts desc */
    /* Use effective_fg_recency(): the raw field is refreshed by background noise and could out-rank a genuinely used app. */
    time_t rec_a = effective_fg_recency(pa), rec_b = effective_fg_recency(pb);
    if (rec_a != rec_b)
        return cmp3(rec_a, rec_b);                    /* older first */
    return cmp3(pb->rss_kb, pa->rss_kb);              /* larger RSS first */
}

/* ZRAM kill comparator: rank desc, blended swap desc, score, restart_count, recency; swap blends avg_swap_kb (EMA) 70/30 with live swap_kb for stable ordering. */
static int cmp_killable_zram(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    int ra = kill_rank_key(pa->rank), rb = kill_rank_key(pb->rank);
    if (ra != rb)
        return cmp3(rb, ra);                          /* rank desc */
    long sw_a = pa->avg_swap_kb > 0
                ? (pa->avg_swap_kb * 70 + pa->swap_kb * 30) / 100
                : pa->swap_kb;
    long sw_b = pb->avg_swap_kb > 0
                ? (pb->avg_swap_kb * 70 + pb->swap_kb * 30) / 100
                : pb->swap_kb;
    if (sw_a != sw_b) return cmp3(sw_b, sw_a);        /* blended swap desc */
    if (pa->score != pb->score)
        return cmp3(pa->score, pb->score);
    if (pa->restart_count != pb->restart_count)
        return cmp3(pa->restart_count, pb->restart_count);
    /* Use the trusted effective_fg_recency() choke point, not the raw field. */
    time_t rec_a = effective_fg_recency(pa), rec_b = effective_fg_recency(pb);
    if (rec_a != rec_b)
        return cmp3(rec_a, rec_b);
    return cmp3(pb->rss_kb, pa->rss_kb);
}

/* RAM pressure kill */

#define SQUEEZE_COOLDOWN_S         60

#define VM_TUNE_AVAIL_MID_PCT      15   /* below this: mid-pressure tier */
#define VM_TUNE_AVAIL_LOW_PCT      10   /* below this: low-pressure tier (the 25.4%-kswapd band) */

#define VM_TUNE_EXTRA_FREE_KB_NORMAL   0        /* default, no extra headroom requested */
#define VM_TUNE_EXTRA_FREE_KB_MID      100000   /* ~100MB */
#define VM_TUNE_EXTRA_FREE_KB_LOW      200000   /* ~200MB */

#define VM_TUNE_WATERMARK_SCALE_NORMAL  10   /* kernel default */
#define VM_TUNE_WATERMARK_SCALE_MID     100
#define VM_TUNE_WATERMARK_SCALE_LOW     200

/* Dwell before acting on a tier change, so an avail_pct hugging a boundary doesn't write-storm the vm sysctls. */
#define VM_TUNE_DWELL_S   15

static int  g_vm_tune_tier    = -1;   /* -1 = uninitialized, forces first-tick apply */
static time_t g_vm_tune_pending_since = 0;
static int  g_vm_tune_pending_tier    = -1;

static void vm_tune_write_int(const char *path, long val) {
    FILE *f = fopen(path, "w");
    if (!f) { logw("VmTune: cannot open %s: %s", path, strerror(errno)); return; }
    fprintf(f, "%ld\n", val);
    fclose(f);
}

/* Drops only clean page cache (echo 1): the cheapest option, and unlike dentry/inode drops it doesn't slow later lookups. */
static void drop_clean_caches(const char *reason) {
    static time_t last_drop = 0;
    time_t now = time(NULL);
    if (now - last_drop < 20) return;   /* hard floor, never spam even if called back-to-back */
    last_drop = now;
    vm_tune_write_int("/proc/sys/vm/drop_caches", 1);
    logi("DropCaches: clean page cache dropped (%s)", reason);
}

/* Applies extra_free_kbytes/watermark_scale_factor per tier, gated by VM_TUNE_DWELL_S; also drops clean cache on entering the low tier and drives kswapd pin/unpin by ZRAM tier. */
static void apply_avail_tier_tunables(int avail_pct, int zram_used_pct) {
    int tier;   /* 0=normal 1=mid 2=low */
    if (avail_pct < VM_TUNE_AVAIL_LOW_PCT)       tier = 2;
    else if (avail_pct < VM_TUNE_AVAIL_MID_PCT)  tier = 1;
    else                                          tier = 0;

    time_t now = time(NULL);
    if (g_vm_tune_tier < 0) {
        /* first tick, apply immediately, nothing to dwell against yet */
        g_vm_tune_tier = tier;
        g_vm_tune_pending_tier = tier;
        g_vm_tune_pending_since = now;
    } else if (tier != g_vm_tune_tier) {
        if (tier != g_vm_tune_pending_tier) {
            /* tier just changed to something new, start the dwell clock */
            g_vm_tune_pending_tier = tier;
            g_vm_tune_pending_since = now;
        } else if (now - g_vm_tune_pending_since >= VM_TUNE_DWELL_S) {
            g_vm_tune_tier = tier;
        }
    } else {
        /* back to the already-active tier, cancel any pending switch */
        g_vm_tune_pending_tier = tier;
        g_vm_tune_pending_since = now;
    }

    static int last_applied_tier = -1;
    if (g_vm_tune_tier != last_applied_tier) {
        long extra_free, wmark;
        switch (g_vm_tune_tier) {
            case 2:  extra_free = VM_TUNE_EXTRA_FREE_KB_LOW; wmark = VM_TUNE_WATERMARK_SCALE_LOW; break;
            case 1:  extra_free = VM_TUNE_EXTRA_FREE_KB_MID; wmark = VM_TUNE_WATERMARK_SCALE_MID; break;
            default: extra_free = VM_TUNE_EXTRA_FREE_KB_NORMAL; wmark = VM_TUNE_WATERMARK_SCALE_NORMAL; break;
        }
        vm_tune_write_int("/proc/sys/vm/extra_free_kbytes", extra_free);
        vm_tune_write_int("/proc/sys/vm/watermark_scale_factor", wmark);
        logi("VmTune: avail=%d%% -> tier=%d  extra_free_kbytes=%ldKB  watermark_scale_factor=%ld",
             avail_pct, g_vm_tune_tier, extra_free, wmark);
        last_applied_tier = g_vm_tune_tier;
        if (g_vm_tune_tier == 2) drop_clean_caches("entered low-avail tier");
    }

    time_t now_kp = time(NULL);
    bool want_unpinned = (zram_used_pct >= ZRAM_CRIT_PCT) ||
                          (now_kp < g_kswapd_event_unpin_until);
    if (want_unpinned) {
        if (g_kswapd_pinned) {
            unpin_kswapd();
            g_kswapd_last_transition_t = now_kp;
        }
    } else if (zram_used_pct < ZRAM_STUCK_PCT) {
        if (!g_kswapd_pinned &&
            (now_kp - g_kswapd_last_transition_t) >= KSWAPD_PIN_DWELL_S) {
            repin_kswapd_if_needed();
            g_kswapd_last_transition_t = now_kp;
        }
    }
}

static int graduated_squeeze_target_pct(void) {
    int z = g_zram_used_pct;

    if (z < 50)              return 65;  /* plenty of ZRAM headroom, full squeeze */
    if (z < 70)              return 75;  /* moderate */
    if (z < ZRAM_WARN_PCT)   return 90;  /* gentle, taper off as headroom shrinks */
    return 0;                             /* at/above WARN_PCT, no more room to push into */
}
#define RECLAIM_MIN_RSS_KB    20480  /* only worth trying on apps holding >=20MB */
#define RECLAIM_MIN_FREED_KB   5120  /* must free >=5MB to count as a real reprieve */
#define RECLAIM_COOLDOWN_S      600  /* don't retry the same app within 10min */
#define KILL_ROI_MIN_RSS_KB   15360  /* below this, RAM gain from killing is marginal */
#define KILL_ROI_RC_THRESHOLD     5  /* restart_count >= this = expensive/thrashy to relaunch */

#define CGROUP_PATH_CACHE_SIZE 32
typedef struct { pid_t pid; char path[192]; bool valid; unsigned long long starttime; } CgroupPathCacheEntry;
static CgroupPathCacheEntry g_cgroup_path_cache[CGROUP_PATH_CACHE_SIZE];

static bool cgroup_path_for_pid_fresh(pid_t pid, char *out, size_t out_sz) {
    char procpath[64], line[320];
    snprintf(procpath, sizeof(procpath), "/proc/%d/cgroup", pid);
    FILE *f = fopen(procpath, "r");
    if (!f) return false;
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::", 3) == 0) {
            char *p = line + 3;
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            snprintf(out, out_sz, "/sys/fs/cgroup%s", p);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static void cgroup_path_cache_invalidate(pid_t pid) {
    for (int i = 0; i < CGROUP_PATH_CACHE_SIZE; i++)
        if (g_cgroup_path_cache[i].valid && g_cgroup_path_cache[i].pid == pid)
            g_cgroup_path_cache[i].valid = false;
}

static bool cgroup_path_for_pid(pid_t pid, char *out, size_t out_sz) {

    for (int i = 0; i < CGROUP_PATH_CACHE_SIZE; i++) {
        if (g_cgroup_path_cache[i].valid && g_cgroup_path_cache[i].pid == pid) {
            unsigned long long cur_start = read_proc_starttime(pid);
            if (cur_start != 0 && cur_start == g_cgroup_path_cache[i].starttime) {
                snprintf(out, out_sz, "%s", g_cgroup_path_cache[i].path);
                return true;
            }
            /* starttime mismatch (or process gone): the PID was recycled or is unreadable, so re-resolve fresh instead of trusting the cache. */
            g_cgroup_path_cache[i].valid = false;
            break;
        }
    }
    if (!cgroup_path_for_pid_fresh(pid, out, out_sz)) return false;
    static int cache_next = 0;
    snprintf(g_cgroup_path_cache[cache_next].path,
             sizeof(g_cgroup_path_cache[cache_next].path), "%s", out);
    g_cgroup_path_cache[cache_next].pid = pid;
    g_cgroup_path_cache[cache_next].starttime = read_proc_starttime(pid);
    g_cgroup_path_cache[cache_next].valid = true;
    cache_next = (cache_next + 1) % CGROUP_PATH_CACHE_SIZE;
    return true;
}

static bool cgroup_write(pid_t pid, const char *file, const char *val) {
    char base[192], path[224];
    if (!cgroup_path_for_pid(pid, base, sizeof(base))) return false;
    snprintf(path, sizeof(path), "%s/%s", base, file);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        /* Cached path may be stale: invalidate and retry once with a fresh resolve. */
        cgroup_path_cache_invalidate(pid);
        if (!cgroup_path_for_pid_fresh(pid, base, sizeof(base))) return false;
        snprintf(path, sizeof(path), "%s/%s", base, file);
        fd = open(path, O_WRONLY);
        if (fd < 0) return false;
    }
    ssize_t w = write(fd, val, strlen(val));
    close(fd);
    return w == (ssize_t)strlen(val);
}

#define LOGCAT_DRAIN_MAX_LINES 40

/* logcat runs via pipe2()+fork()+execvp(), not popen(): pclose() can block forever on a quiet logcat and strand orphans on --stop.
 * With the pid kept we SIGTERM, wait, then SIGKILL; PR_SET_PDEATHSIG covers a daemon crash. */
typedef struct {
    FILE *fp;      /* NULL = not running */
    pid_t pid;     /* <0 = no live child */
    bool  failed;  /* give up for this run after a spawn/EOF failure */
} LogcatWatch;

static LogcatWatch g_logcat_sys    = { NULL, -1, false };
static LogcatWatch g_logcat_events = { NULL, -1, false };

static bool logcat_spawn(LogcatWatch *w, char *const argv[]) {
    int pfd[2];
    if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC) != 0) return false;

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        /* Child: reset signal dispositions to default before exec; the daemon's handlers only set flags and would swallow our shutdown SIGTERM. */
        close(pfd[0]);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        int nfd = open("/dev/null", O_RDWR);
        if (nfd >= 0) {
            dup2(nfd, STDIN_FILENO);
            dup2(nfd, STDERR_FILENO);   /* was "2>/dev/null" in the popen cmd */
            if (nfd > STDERR_FILENO) close(nfd);
        }
        if (pfd[1] != STDOUT_FILENO) {
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
        }
        prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
        if (getppid() != parent_pid) _exit(127);  /* parent died mid-fork */
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pfd[1]);
    FILE *fp = fdopen(pfd[0], "r");
    if (!fp) {
        close(pfd[0]);
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
        return false;
    }
    w->fp  = fp;
    w->pid = pid;
    return true;
}

/* Kill + reap a logcat child without blocking indefinitely: SIGTERM, a bounded ~1 s WNOHANG poll, then SIGKILL + blocking waitpid. */
static void logcat_watch_stop(LogcatWatch *w) {
    if (w->fp) { fclose(w->fp); w->fp = NULL; }
    if (w->pid > 0) {
        kill(w->pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 100 && !reaped; i++) {
            pid_t r = waitpid(w->pid, NULL, WNOHANG);
            if (r == w->pid) {
                reaped = true;
            } else if (r < 0) {
                if (errno == EINTR) continue;
                break;   /* ECHILD, already reaped elsewhere */
            } else {
                usleep(10000);
            }
        }
        if (!reaped) {
            kill(w->pid, SIGKILL);
            while (waitpid(w->pid, NULL, 0) < 0 && errno == EINTR) { }
        }
        w->pid = -1;
    }
}

static void logcat_watch_init(void) {
    if (!g_logcat_sys.fp && !g_logcat_sys.failed) {
        char *const argv[] = { "logcat", "-b", "system", "-v", "brief",
                               "ActivityManager:I", "lowmemorykiller:V",
                               "*:S", NULL };
        if (logcat_spawn(&g_logcat_sys, argv)) {
            logi("ExternalKill: logcat attribution armed (system)");
        } else {
            logw("ExternalKill: logcat spawn failed — attribution disabled "
                 "for this run (%s)", strerror(errno));
            g_logcat_sys.failed = true;
        }
    }
    if (!g_logcat_events.fp && !g_logcat_events.failed) {
        char *const argv[] = { "logcat", "-b", "events", "-v", "brief",
                               "am_kill:I", "am_proc_died:I",
                               "am_low_memory:I", "*:S", NULL };
        if (logcat_spawn(&g_logcat_events, argv)) {
            logi("ExternalKill: logcat attribution armed (events, "
                 "UNVERIFIED tag names — confirm with raw capture)");
        } else {
            logw("ExternalKill: logcat events-buffer spawn failed — "
                 "events-buffer attribution disabled for this run (%s)",
                 strerror(errno));
            g_logcat_events.failed = true;
        }
    }
}

static bool pkg_name_matches_line(const char *line, const char *name) {
    size_t nlen = strlen(name);
    if (nlen == 0) return false;
    const char *p = line;
    while ((p = strstr(p, name)) != NULL) {
        char before = (p == line) ? '\0' : *(p - 1);
        char after  = p[nlen];
        bool before_ok = !(isalnum((unsigned char)before) || before == '_' || before == '.');
        bool after_ok  = !(isalnum((unsigned char)after)  || after  == '_' || after  == '.');
        if (before_ok && after_ok) return true;
        p += 1; /* false match, keep scanning the rest of the line */
    }
    return false;
}

/* Shared by logcat_kill_watch() (system buffer) and logcat_kill_watch_events() (events buffer): drain one watch, match kill-looking lines against live tracked apps, log and record. */
static void logcat_drain_and_attribute(LogcatWatch *w,
                                        ProcInfo *tbl, int cnt,
                                        const char *source_label) {
    if (!w->fp) return;
    char line[512];
    int drained = 0;
    while (drained < LOGCAT_DRAIN_MAX_LINES) {
        if (!fgets(line, sizeof(line), w->fp)) {
            if (feof(w->fp)) {
                /* Child exited: close and reap it so it can't linger as a zombie, then disable this source for the run. */
                logcat_watch_stop(w);
                w->failed = true;
                logw("ExternalKill: logcat %s stream ended — attribution "
                     "disabled for this run", source_label);
            } else {
                clearerr(w->fp); /* benign EAGAIN, no data right now */
            }
            break;
        }
        drained++;
        if (!strstr(line, "Kill") && !strstr(line, "kill") &&
            !strstr(line, "died")) continue;
        for (int i = 0; i < cnt; i++) {
            if (tbl[i].rank == RANK_EXEMPT) continue;
            if (pkg_name_matches_line(line, tbl[i].name)) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                logi("ExternalKill: [%s] %s: %s", tbl[i].name, source_label, line);
                record_kill(&tbl[i]);
                break;
            }
        }
    }
}

/* Drains buffered logcat output (bounded per tick) and matches kill-looking lines against live tracked apps; observation only, never a kill decision. */
static void logcat_kill_watch(ProcInfo *tbl, int cnt) {
    logcat_watch_init();

    logcat_drain_and_attribute(&g_logcat_sys, tbl, cnt, "system");
    logcat_drain_and_attribute(&g_logcat_events, tbl, cnt, "events");
}

#define UNATTRIB_SEEN_WINDOW_S       6
#define UNATTRIB_RECENT_KILL_S      20

static void unattrib_check_vanished(ProcInfo *tbl, int cnt, time_t now) {
    for (int s = 0; s < g_score_count; s++) {
        AppScore *as = &g_scores[s];
        if (as->last_seen_t == 0) continue;
        time_t since_seen = now - as->last_seen_t;
        if (since_seen <= 0 || since_seen > UNATTRIB_SEEN_WINDOW_S) continue;

        bool still_present = false;
        for (int i = 0; i < cnt; i++) {
            if (strcmp(tbl[i].name, as->name) == 0) { still_present = true; break; }
        }
        if (still_present) continue; /* normal case, mark_seen below refreshes it */

        if (as->category == 0) continue;

        bool already_attributed = false;
        for (int k = 0; k < KILL_HIST_SIZE; k++) {
            if (g_kill_hist[k].name[0] &&
                strcmp(g_kill_hist[k].name, as->name) == 0 &&
                (now - g_kill_hist[k].killed_at) < UNATTRIB_RECENT_KILL_S) {
                already_attributed = true;
                break;
            }
        }
        if (already_attributed) continue;

        logw("Unattributed: [%s] vanished (last seen %llds ago, tier=%d "
             "score=%d) — no kill/ExternalKill record for it",
             as->name, (long long)since_seen, as->rank_tier, score_compute(as));
        ProcInfo ghost = {0};
        strncpy(ghost.name, as->name, sizeof(ghost.name) - 1);
        record_kill(&ghost);
    }
}

static void unattrib_mark_seen(ProcInfo *tbl, int cnt, time_t now) {
    for (int i = 0; i < cnt; i++) {
        AppScore *as = score_lookup(tbl[i].name);
        if (as) as->last_seen_t = now;
    }
}

static long try_reclaim(ProcInfo *p, AppScore *as) {
    if (g_zram_used_pct >= ZRAM_WARN_PCT) return 0;
    if (p->rss_kb < RECLAIM_MIN_RSS_KB) return 0;
    time_t now = time(NULL);
    if (as && as->last_reclaim_t > 0 &&
        (now - as->last_reclaim_t) < RECLAIM_COOLDOWN_S)
        return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/reclaim", p->pid);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return 0; /* not supported on this kernel, or process gone */
    ssize_t w = write(fd, "all", 3);
    close(fd);
    if (as) as->last_reclaim_t = now; /* count the attempt either way */
    if (w != 3) return 0;
    long rss_before = p->rss_kb;
    long rss_after = 0, swap_after = 0;
    proc_mem_stats(p->pid, &rss_after, &swap_after);
    if (rss_after <= 0) return 0; /* process gone/unreadable post-reclaim */
    long freed = rss_before - rss_after;
    if (freed < RECLAIM_MIN_FREED_KB) return 0;
    p->rss_kb = rss_after; /* keep cached value consistent for downstream use */
    logi("Reclaim: [%s] freed=%ldMB (rss %ldMB->%ldMB)",
         p->name, freed/1024, rss_before/1024, rss_after/1024);
    kswapd_note_disruptive_event();
    return freed;
}

static bool try_squeeze_then_reclaim(ProcInfo *p, long *freed_out) {
    if (freed_out) *freed_out = 0;
    /* Skip PRIO_JUNK: already the lowest-value candidates, not worth the write+reread. */
    if (p->prio == PRIO_JUNK) return false;

    AppScore *cas = score_lookup(p->name);
    /* Try the cgroup v2 memory.high squeeze first: it reclaims synchronously inside write() (78MB->46MB seen on-device) and pushes pages into ZRAM, the only swap here. */
    int squeeze_pct = graduated_squeeze_target_pct();
    long high_target_kb = squeeze_pct > 0 ? (p->rss_kb * squeeze_pct) / 100 : 0;

    bool squeeze_cooling_down = cas && cas->cg_high_set &&
        (time(NULL) - cas->cg_high_set_t) < SQUEEZE_COOLDOWN_S;
    if (high_target_kb > 0 && !squeeze_cooling_down) {
        char valbuf[32];
        snprintf(valbuf, sizeof(valbuf), "%ld", high_target_kb * 1024);
        if (cgroup_write(p->pid, "memory.high", valbuf)) {
            if (cas) {
                if (!cas->cg_high_set) cas->cg_high_set_t = time(NULL);
                cas->cg_high_set = true;
            }
            long rss_after = 0, swap_after = 0;
            proc_mem_stats(p->pid, &rss_after, &swap_after);
            long freed = (rss_after > 0) ? (p->rss_kb - rss_after) : 0;
            if (freed >= RECLAIM_MIN_FREED_KB) {
                if (cas) cas->cg_high_effective = true;
                if (freed_out) *freed_out = freed;
                p->rss_kb = rss_after;
                logi("CgSqueeze: [%s] freed=%ldMB via memory.high (target=%d%%)",
                     p->name, freed/1024, squeeze_pct);
                kswapd_note_disruptive_event();
                return true;
            }
            /* The limit was installed but reclaimed nothing meaningful: mark it ineffective so the safety net releases it after SQUEEZE_HIGH_FAILED_RELEASE_S, then fall through to reclaim. */
            if (cas) cas->cg_high_effective = false;
            logi("CgSqueeze: [%s] memory.high installed but freed <%dMB — "
                 "short-release timer armed",
                 p->name, RECLAIM_MIN_FREED_KB/1024);
        } else {
            /* Install failed: a previous limit (cg_high_set already true) stays tracked, never lose that state. */
            logw("CgSqueeze: [%s] memory.high write failed — limit %s",
                 p->name,
                 (cas && cas->cg_high_set) ? "unchanged (previous still active)"
                                           : "not applied");
        }
    }
    long reclaimed = try_reclaim(p, cas);
    if (reclaimed > 0) {
        if (freed_out) *freed_out = reclaimed;
        return true;
    }
    return false;
}

static time_t g_last_pin_squeeze_t = 0;

#define SQUEEZE_PIN_INTERVAL_S     60
#define SQUEEZE_PIN_SKIP_ACTIVE_S  30
#define SQUEEZE_HIGH_MAX_AGE_S    (15 * 60)
#define SQUEEZE_HIGH_FAILED_RELEASE_S 90  /* ineffective limit max age */

static void pin_squeeze_pass(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    if (now - g_last_pin_squeeze_t < SQUEEZE_PIN_INTERVAL_S) return;
    g_last_pin_squeeze_t = now;

    /* Release lingering memory.high limits when ZRAM < 50% (the curve is flat-zero), after SQUEEZE_HIGH_FAILED_RELEASE_S (90 s) for an ineffective squeeze, or at the SQUEEZE_HIGH_MAX_AGE_S (15 min) cap. */
    for (int i = 0; i < g_score_count; i++) {
        AppScore *as = &g_scores[i];
        if (!as->cg_high_set || as->cg_high_set_t <= 0) continue;

        bool pressure_eased = g_zram_used_pct < 50;
        int  max_age_s = as->cg_high_effective ? SQUEEZE_HIGH_MAX_AGE_S
                                               : SQUEEZE_HIGH_FAILED_RELEASE_S;
        if (!pressure_eased && (now - as->cg_high_set_t) < max_age_s) continue;
        const char *why = pressure_eased ? "pressure-eased" :
                          (as->cg_high_effective ? "15-minute"
                                                 : "ineffective-squeeze");
        bool process_found = false;
        bool released = false;
        for (int j = 0; j < cnt; j++) {
            if (strcmp(tbl[j].name, as->name) != 0) continue;
            process_found = true;
            released = cgroup_write(tbl[j].pid, "memory.high", "max");
            break;
        }
        if (released || !process_found) {
            as->cg_high_set = false;
            as->cg_high_set_t = 0;
            as->cg_high_effective = false;
            logi("SqueezeRelease: [%s] memory.high cleared by %s safety-net",
                 as->name, why);
        } else {
            logw("SqueezeRelease: [%s] %s safety-net write failed; will retry",
                 as->name, why);
        }
    }

    /* Cheap early-out: the graduated curve returns 0 below 50%, so skip the table walk. */
    if (g_zram_used_pct < 50) return;

    int squeezed = 0;
    for (int i = 0; i < g_prev_pinned_n; i++) {
        ProcInfo *p = NULL;
        for (int j = 0; j < cnt; j++) {
            if (strcmp(tbl[j].name, g_prev_pinned[i]) == 0) { p = &tbl[j]; break; }
        }
        if (!p) continue;
        AppScore *as = score_lookup(p->name);
        if (!as || !as->cg_low_set) continue; /* only actually-pinned right now */
        time_t recency = effective_fg_recency(p);
        if (recency != 0 && (now - recency) < SQUEEZE_PIN_SKIP_ACTIVE_S) continue;
        /* Cooldown is enforced inside try_squeeze_then_reclaim() (SQUEEZE_COOLDOWN_S, shared by all callers). */
        long freed = 0;
        if (try_squeeze_then_reclaim(p, &freed)) {
            squeezed++;
            logi("PinSqueeze: [%s] freed=%ldMB (periodic pass)", p->name, freed/1024);
        }
    }
    if (squeezed > 0)
        logi("PinSqueeze: %d/%d pinned app(s) squeezed this pass",
             squeezed, g_prev_pinned_n);
}

#define SQUEEZE_KILL_GRACE_S   45

/* is_critical means avail_pct < FREE_CRIT_PCT only and alone skips the squeeze/ROI gates; RAM_LOW still passes the grace window, squeeze and kill-ROI gate. */
static bool should_spare_kill(ProcInfo *p, bool is_critical, long *freed_out) {
    if (freed_out) *freed_out = 0;
    if (is_critical) return false;

    time_t now_sk = time(NULL);
    AppScore *sk_as = score_lookup(p->name);
    if (sk_as &&
        ((sk_as->cg_high_set     && (now_sk - sk_as->cg_high_set_t)   < SQUEEZE_KILL_GRACE_S) ||
         (sk_as->last_reclaim_t > 0 && (now_sk - sk_as->last_reclaim_t) < SQUEEZE_KILL_GRACE_S))) {
        return true;
    }

    /* Squeeze before killing: a real reprieve keeps the app warm instead of forcing a cold relaunch. */
    if (try_squeeze_then_reclaim(p, freed_out)) return true;

    /* Kill ROI gate: don't spend a costly relaunch (high restart_count) on a candidate holding barely any RAM. */
    if (p->restart_count >= KILL_ROI_RC_THRESHOLD &&
        p->rss_kb < KILL_ROI_MIN_RSS_KB) {
        time_t now = time(NULL);
        if (now - g_kill_roi_log_t >= LOG_RATELIMIT_S) {
            g_kill_roi_log_t = now;
            logi("KillROI: sparing [%s] rss=%ldMB rc=%d (below ROI floor)",
                 p->name, p->rss_kb/1024, p->restart_count);
        }
        return true;
    }
    return false;
}

static long do_kill(ProcInfo *tbl, int cnt, long target_kb,
                    Priority min_prio, bool is_critical, int avail_pct) {
    if (cnt > 1) qsort(tbl, (size_t)cnt, sizeof(ProcInfo), cmp_killable_ram);
    long freed = 0;
    time_t now = time(NULL);
    for (int i = 0; i < cnt && freed < target_kb; i++) {
        ProcInfo *p = &tbl[i];
        if (p->prio < min_prio)                  continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)        continue;
        if (!is_critical && p->prio != PRIO_JUNK && is_actively_retained(p->name)) {
            logi("Retained: sparing %s (pinned by retention)", p->name);
            continue;
        }

        time_t rec_dk = effective_fg_recency(p);
        if (!is_critical && rec_dk > 0 &&
            (now - rec_dk) < score_protect_window_ram(p->name, avail_pct)) {
            logi("Sparing %s (score=%d win=%ds age=%lds)",
                 p->name, p->score,
                 score_protect_window_ram(p->name, avail_pct),
                 now - rec_dk);
            continue;
        }
        if (!is_critical && recently_killed(p->name)) {
            logi("Cooldown: skipping %s", p->name); continue;
        }
        /* In CRITICAL, allow PRIO_SEMI_PROTECTED if they hold substantial RAM */
        if (is_critical && p->prio == PRIO_SEMI_PROTECTED && p->rss_kb < 100*1024)
            continue;
        long reclaimed = 0;
        if (should_spare_kill(p, is_critical, &reclaimed)) {
            freed += reclaimed;
            continue;
        }
        int krc = kill_proc_verified(p);
        if (krc == 0) {
            freed += p->rss_kb;
            record_kill(p);
            logk("RAM pid=%-6d prio=%d sc=%-4d rc=%-2d fg_age=%-4lds rss=%4ldMB sw=%4ldMB [%s]",
                 p->pid, p->prio, p->score, p->restart_count,
                 p->raw_last_fg_do_not_read_directly ? (now - p->raw_last_fg_do_not_read_directly) : -1,
                 p->rss_kb/1024, p->avg_swap_kb/1024, p->name);
            usleep(30000);
        }
        /* krc == 1: identity changed/gone, stale entry, skip.
         * krc == -1: signal failed (EPERM...), skip, same as before. */
    }
    return freed;
}

/* Full reset sweep follows the normal kill policy, not a bypass: the score protect window, foreground/visible, rank-exempt, cooldown, pinned state, RESET_SPARE_HOT_N hot spares,
 * the minimum-benefit floor and RESET_MAX_KILLS all apply; the skip counters in the summary line explain each decision. */
#define RESET_MIN_RSS_KB       (20 * 1024)
#define RESET_MAX_KILLS        24
#define RESET_SPARE_HOT_N      2

static void full_reset_sweep(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    if (g_screen_off_since == 0 ||
        (now - g_screen_off_since) < IDLE_DEEP_S)
        return;

    time_t base = g_last_full_reset_t ? g_last_full_reset_t : g_start_time;
    if ((now - base) < RESET_GATE_S)
        return;

    g_reset_in_progress = true;
    int  killed = 0, skipped_cd = 0, skipped_ret = 0;
    int  skipped_hot = 0, skipped_small = 0, skipped_stale = 0;
    int  skipped_recent = 0;
    long freed_kb = 0;
    int  hot_kept = 0;

    for (int i = 0; i < cnt && killed < RESET_MAX_KILLS; i++) {
        ProcInfo *p = &tbl[i];
        if (p->prio == PRIO_NEVER)          continue;
        if (p->rank == RANK_EXEMPT)         continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)  continue; /* visible/fg safety net */
        if (is_never_kill_now(p))           continue; /* true fg, never */

        /* cooldown after kill, shared with every other kill path */
        if (recently_killed(p->name)) { skipped_cd++; continue; }

        /* retention/pinned: like the adaptive T3 override, only critical RAM outranks retention, and a sweep is never critical. */
        if (is_actively_retained(p->name) && p->prio != PRIO_JUNK) {
            logi("ResetSkip: [%s] reason=active_retention", p->name);
            skipped_ret++; continue;
        }

        /* Same recency + score protection as do_kill(): inside the score-scaled protect window the app isn't stale; a sweep is never critical, so the FREE_HIGH_PCT window applies. */
        time_t rec_dk = effective_fg_recency(p);
        if (rec_dk > 0) {
            int win = score_protect_window(p->name);
            if ((now - rec_dk) < win) {
                logi("ResetSkip: [%s] reason=%s score=%d win=%ds age=%lds",
                     p->name,
                     p->score >= SCORE_MAX / 2 ? "score_protected"
                                               : "recently_used",
                     p->score, win, (long)(now - rec_dk));
                skipped_recent++; continue;
            }
        }

        /* keep a couple of tier1-background apps so light multitasking survives (like T3_SPARE_HOT_N) */
        if (is_tier1_background(p) && hot_kept < RESET_SPARE_HOT_N) {
            logi("ResetSkip: [%s] reason=hot_survivor", p->name);
            skipped_hot++; hot_kept++; continue;
        }

        /* minimum benefit floor: relaunching a tiny process costs more than it reclaims */
        if (p->rss_kb + p->swap_kb < RESET_MIN_RSS_KB) {
            logi("ResetSkip: [%s] reason=below_min_rss rss+swap=%ldKB",
                 p->name, p->rss_kb + p->swap_kb);
            skipped_small++; continue;
        }

        int krc = kill_proc_verified(p);
        if (krc == 0) {
            freed_kb += p->rss_kb;
            killed++;
            record_kill(p);
            logk("RESET pid=%-6d prio=%d sc=%-4d rc=%-2d rss=%4ldMB sw=%4ldMB [%s]",
                 p->pid, p->prio, p->score, p->restart_count,
                 p->rss_kb/1024, p->avg_swap_kb/1024, p->name);
            usleep(30000);
        } else if (krc == 1) {
            skipped_stale++;
        }
    }
    g_reset_in_progress = false;
    g_last_full_reset_t = now;
    logi("Reset: sweep complete — killed=%d freed=%ldMB "
         "(skipped: cd=%d retained=%d recent=%d hot=%d small=%d stale=%d), "
         "next eligible in %dh",
         killed, freed_kb/1024, skipped_cd, skipped_ret, skipped_recent,
         skipped_hot, skipped_small, skipped_stale, RESET_GATE_S/3600);
}

/* ZRAM pressure kill */
static void zram_pressure_kill(ProcInfo *tbl, int cnt, int zram_used_pct) {
    if (zram_used_pct < ZRAM_TRIM_PCT) {

        if (g_zram_kill_start_t != 0) { g_zram_kill_start_t = 0; g_zram_pct_at_start = 0; }
        return;
    }

    time_t now = time(NULL);

    if ((now - g_zram_kill_last_t) < ZRAM_KILL_INTVL_S) return;
    if (g_zram_pause_until > 0 && now < g_zram_pause_until) return;

    if (g_zram_kill_start_t == 0) {
        g_zram_kill_start_t = now;
        g_zram_pct_at_start = zram_used_pct;
    } else {
        long campaign_s = now - g_zram_kill_start_t;
        if (campaign_s >= FUTILITY_WINDOW_S) {
            int drop = g_zram_pct_at_start - zram_used_pct;
            if (drop < FUTILITY_MIN_DROP) {
                logi("ZRAMkill: futile (%d%%→%d%% in %lds) — pausing %ds",
                     g_zram_pct_at_start, zram_used_pct,
                     campaign_s, FUTILITY_PAUSE_S);
                g_zram_pause_until  = now + FUTILITY_PAUSE_S;
                g_zram_kill_start_t = 0;
                return;
            }
            g_zram_kill_start_t = now;
            g_zram_pct_at_start = zram_used_pct;
        }
    }

    g_zram_kill_last_t = now;
    if (cnt > 1) qsort(tbl, (size_t)cnt, sizeof(ProcInfo), cmp_killable_zram);
    long freed_swap = 0;
    int  kills = 0, skipped_cd = 0;

    for (int i = 0; i < cnt && kills < ZRAM_KILL_BATCH; i++) {
        ProcInfo *p = &tbl[i];
        if (p->rank == RANK_EXEMPT)                  continue;
        if (p->prio < PRIO_BACKGROUND)              continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)           continue;
        if (p->swap_kb < 256)                        continue;

        bool zram_critical_retain = zram_used_pct >= ZRAM_CRIT_PCT;
        if (is_actively_retained(p->name) && p->prio != PRIO_JUNK &&
            !zram_critical_retain) continue;
        if (zram_critical_retain && is_actively_retained(p->name) &&
            p->prio != PRIO_JUNK) {
            logw("ZRAMkill: ZRAM critical (%d%%) — overriding retention for [%s]",
                 zram_used_pct, p->name);
        }

        {
            int win = score_protect_window(p->name);

            time_t rec_zk = effective_fg_recency(p);
            if (rec_zk > 0 && (now - rec_zk) < win) continue;
        }
        if (recently_killed(p->name)) { skipped_cd++; continue; }

        if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
            p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB &&
            p->prio != PRIO_JUNK)                   continue;
        if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
            p->swap_kb < 200*1024)                  continue;

        if (p->restart_count >= KILL_ROI_RC_THRESHOLD &&
            p->rss_kb < KILL_ROI_MIN_RSS_KB) {
            if (now - g_kill_roi_log_t >= LOG_RATELIMIT_S) {
                g_kill_roi_log_t = now;
                logi("KillROI: sparing [%s] rss=%ldMB rc=%d (below ROI floor)",
                     p->name, p->rss_kb/1024, p->restart_count);
            }
            continue;
        }
        int krc = kill_proc_verified(p);
        if (krc == 0) {
            freed_swap += p->swap_kb;
            kills++;
            record_kill(p);
            logk("ZRAM%d%% pid=%-6d swap=%4ldMB avg=%4ldMB rc=%-2d [%s]",
                 zram_used_pct, p->pid,
                 p->swap_kb/1024, p->avg_swap_kb/1024,
                 p->restart_count, p->name);
            usleep(40000);
        }
    }

    if (skipped_cd > 0 && kills == 0) {
        if (now - g_log_zramcd_t >= LOG_RATELIMIT_S) {
            g_log_zramcd_t = now;
            logi("ZRAMcd: %d candidate(s) on cooldown, nothing killed at %d%%",
                 skipped_cd, zram_used_pct);
        }
    }

    /* Emergency pass */
    if (kills == 0 && skipped_cd > 0 && zram_used_pct >= ZRAM_STUCK_PCT) {
        logi("ZRAMemerg: all %d blocked at %d%% — relaxing cd to %ds",
             skipped_cd, zram_used_pct, ZRAM_EMERG_CD_S);
        int emerg_kills = 0;
        for (int i = 0; i < cnt && emerg_kills < ZRAM_EMERG_KILL_MAX; i++) {
            ProcInfo *p = &tbl[i];
            if (p->prio < PRIO_CACHED)           continue;
            if (p->oom_adj <= ADJ_VISIBLE_MAX)    continue;
            if (p->swap_kb < 256)                 continue;
            if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
                p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB &&
                p->prio != PRIO_JUNK)               continue;
            if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
                p->swap_kb < 200*1024)            continue;
            bool blocked = false;
            for (int j = 0; j < KILL_HIST_SIZE; j++) {
                if (g_kill_hist[j].name[0] &&
                    strcmp(g_kill_hist[j].name, p->name) == 0 &&
                    (now - g_kill_hist[j].killed_at) < ZRAM_EMERG_CD_S) {
                    blocked = true; break;
                }
            }
            if (blocked) continue;

            AppScore *emerg_as = score_lookup(p->name);
            if (emerg_as &&
                ((emerg_as->cg_high_set && (now - emerg_as->cg_high_set_t) < SQUEEZE_KILL_GRACE_S) ||
                 (emerg_as->last_reclaim_t > 0 && (now - emerg_as->last_reclaim_t) < SQUEEZE_KILL_GRACE_S))) {
                continue;
            }
            int krc = kill_proc_verified(p);
            if (krc == 0) {
                freed_swap += p->swap_kb;
                emerg_kills++;
                record_kill(p);
                logk("ZRAMemerg pid=%-6d swap=%4ldMB rc=%-2d [%s]",
                     p->pid, p->swap_kb/1024, p->restart_count, p->name);
                usleep(40000);
            }
        }
        if (emerg_kills == 0)
            logi("ZRAMemerg: no eligible target — deep clean will engage if stuck");
    }

    if (freed_swap > 0) {
        usleep(200000);
        int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
        if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
    }
}

static bool has_trim_candidate(ProcInfo *tbl, int cnt) {
    for (int i = 0; i < cnt; i++) {
        if (tbl[i].prio >= PRIO_BACKGROUND && tbl[i].oom_adj > ADJ_VISIBLE_MAX)
            return true;
    }
    return false;
}

#define PROACTIVE_SQUEEZE_MAX_CANDIDATES 256
static long proactive_squeeze_pass(ProcInfo *tbl, int cnt, long want) {
    int  cand_idx[PROACTIVE_SQUEEZE_MAX_CANDIDATES];
    bool done[PROACTIVE_SQUEEZE_MAX_CANDIDATES] = {0};
    int  n = 0;
    for (int i = 0; i < cnt && n < PROACTIVE_SQUEEZE_MAX_CANDIDATES; i++) {
        if (tbl[i].prio >= PRIO_BACKGROUND && tbl[i].oom_adj > ADJ_VISIBLE_MAX)
            cand_idx[n++] = i;
    }
    long squeezed = 0;
    while (squeezed < want) {
        int biggest = -1;
        for (int k = 0; k < n; k++) {
            if (done[k]) continue;
            int i = cand_idx[k];
            if (biggest == -1 || tbl[i].rss_kb > tbl[cand_idx[biggest]].rss_kb)
                biggest = k;
        }
        if (biggest == -1) break;
        done[biggest] = true;
        int i = cand_idx[biggest];
        long freed = 0;
        if (try_squeeze_then_reclaim(&tbl[i], &freed) && freed > 0)
            squeezed += freed;
    }
    return squeezed;
}

static void proactive_squeeze(ProcInfo *tbl, int cnt, MemInfo *mi, ZramInfo *z,
                            const char *new_fg_name) {
    bool zram_hot = z->active && z->used_pct >= ZRAM_TRIM_PCT;
    if (mi->avail_pct >= LAUNCH_TRIM_AVAIL_PCT && !zram_hot)
        return;

    AppScore *as = score_lookup(new_fg_name);
    bool heavy = as && as->avg_swap_kb >= LAUNCH_TRIM_HEAVY_SWAP_KB;
    int target_pct = heavy ? LAUNCH_TRIM_HEAVY_TARGET_PCT : LAUNCH_TRIM_TARGET_PCT;
    if (z->active && z->used_pct >= ZRAM_STUCK_PCT) target_pct += 5;

    /* Warm relaunch (foreground very recently, e.g. a quick recents bounce): its pages are hot and freeing room elsewhere only costs CPU and relaunch thrash;
     * zram_pressure_kill below still runs if ZRAM is hot. */
    bool warm_relaunch = as && as->last_bg > 0 &&
                         (time(NULL) - as->last_bg) < LAUNCH_TRIM_WARM_SKIP_S;
    if (warm_relaunch && !zram_hot) {
        logi("Launch: %s warm relaunch (%lds ago) — skipping RAM squeeze",
             new_fg_name, (long)(time(NULL) - as->last_bg));
        return;
    }

    bool need_free = mi->avail_pct < target_pct && !warm_relaunch;
    if (need_free && !has_trim_candidate(tbl, cnt)) {
        /* Nothing structurally eligible: skip the log line and the squeeze/qsort scans. */
        need_free = false;
    } else if (need_free) {
        logi("Launch: %s foreground%s — proactive squeeze (avail=%d%% zram=%d%% target=%d%%)",
             new_fg_name, heavy ? " [heavy]" : "", mi->avail_pct,
             z->active ? z->used_pct : -1, target_pct);
    }

    if (need_free) {
        long want = (long)g_total_ram_kb * target_pct / 100 - mi->avail_kb;
        if (want > 0) {
            long squeezed = proactive_squeeze_pass(tbl, cnt, want);
            if (squeezed > 0) {
                logi("Launch: proactive squeeze freed %ldMB ahead of %s (no kills)",
                     squeezed/1024, new_fg_name);
                mi->avail_kb += squeezed;
                mi->avail_pct = (int)(mi->avail_kb * 100 / g_total_ram_kb);
            }
            long remaining = want - squeezed;
            if (remaining > 0) {
                /* Squeeze alone didn't cover it: kill only for the shortfall. */
                long freed = do_kill(tbl, cnt, remaining, PRIO_BACKGROUND, false, mi->avail_pct);
                if (freed > 0) {
                    logi("Launch: proactive RAM trim freed %ldMB ahead of %s (squeeze alone fell short)",
                         freed/1024, new_fg_name);
                    g_last_kill_t = time(NULL);
                }
            }
        }
    }

    if (zram_hot)
        zram_pressure_kill(tbl, cnt, z->used_pct);
}

/* Comparator: rank desc, swap_kb desc, adaptive_kill_count asc, score asc */
static int cmp_adaptive_clean(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    int ra = kill_rank_key(pa->rank), rb = kill_rank_key(pb->rank);
    if (ra != rb)
        return cmp3(rb, ra);                          /* rank desc */
    if (pa->swap_kb != pb->swap_kb)
        return cmp3(pb->swap_kb, pa->swap_kb);        /* swap desc */
    AppScore *sa = score_lookup(pa->name);
    AppScore *sb = score_lookup(pb->name);
    int ak_a = sa ? sa->adaptive_kill_count : 0;
    int ak_b = sb ? sb->adaptive_kill_count : 0;
    if (ak_a != ak_b) return cmp3(ak_a, ak_b);        /* fewer kills first */
    if (pa->score != pb->score) return cmp3(pa->score, pb->score);
    return cmp3(pa->raw_last_fg_do_not_read_directly,
                pb->raw_last_fg_do_not_read_directly);/* older first; 0 if equal */
}

static int adaptive_select_tier(MemInfo *mi, ZramInfo *z, bool *do_compact, bool *rising_out) {
    int zpct    = z->used_pct;
    bool ram_low = mi->avail_pct < FREE_LOW_PCT;

    /* single shared definition, see ram_state_critical(). */
    bool ram_critical = ram_state_critical(mi);
    time_t now  = time(NULL);

    /* Trend detection */
    bool rising = false;
    if (g_adaptive_trend_t == 0) {
        g_adaptive_trend_t       = now;
        g_adaptive_prev_zram_pct = zpct;
    } else if ((now - g_adaptive_trend_t) >= ADAPTIVE_TREND_WINDOW_S) {
        int rise = zpct - g_adaptive_prev_zram_pct;
        rising   = (rise >= ADAPTIVE_RISE_THRESH_PCT);
        g_adaptive_prev_zram_pct = zpct;
        g_adaptive_trend_t       = now;
    }
    *rising_out = rising;

    /* Tier selection (IDLE=-2, MAINT=-1, LOW=0, MED=1, HIGH=2, DEEP=3) */
    *do_compact = false;
    int  tier;

    bool deep_eligible =
        (zpct >= ZRAM_STUCK_PCT) &&
        (g_zram_stuck_since > 0) &&
        ((now - g_zram_stuck_since)  >= ZRAM_STUCK_S) &&
        ((now - g_last_deepclean)    >= ZRAM_DEEPCLEAN_CD_S) &&
        !(g_deepclean_pause_until > 0 && now < g_deepclean_pause_until);

    /* Phone idle: no fg-app change for IDLE_DETECT_S and ZRAM not elevated */
    bool phone_idle = (g_last_fg_change_t > 0) &&
                      ((now - g_last_fg_change_t) >= IDLE_DETECT_S) &&
                      (zpct < (100 - IDLE_MIN_ZRAM_FREE_PCT)) &&
                      !ram_low;

    /* Screen off >= 30 min: intense idle clean, sweep harder and compact even without ZRAM pressure. */
    bool idle_deep_eligible = (g_screen_off_since > 0) &&
                              ((now - g_screen_off_since) >= IDLE_DEEP_S);

    if (deep_eligible) {
        tier = 3; *do_compact = true;
    } else if (ram_critical) {
        tier = 2;
        if (!(zpct >= ZRAM_STUCK_PCT || (zpct >= ZRAM_TRIM_PCT && rising)))
            logi("AdaptiveTier: avail=%d%% < FREE_CRIT_PCT(%d%%) forced tier=2 "
                 "independent of ZRAM(%d%%)", mi->avail_pct, FREE_CRIT_PCT, zpct);
    } else if (zpct >= ZRAM_STUCK_PCT || (zpct >= ZRAM_TRIM_PCT && rising)) {
        tier = 2;
    } else if (zpct >= ZRAM_TRIM_PCT || rising || ram_low) {
        tier = 1;
    } else if (zpct >= ZRAM_WARN_PCT) {
        tier = 0;
    } else if (idle_deep_eligible) {
        tier = -3; *do_compact = true; /* IDLE-DEEP: intense + compaction */
    } else if (phone_idle) {
        tier = -2; /* IDLE */
    } else {
        tier = -1; /* MAINTENANCE */
    }

    /* PSI escalation: bump the tier when kernel PSI shows a real memory stall beyond ZRAM%. */
    {
        double psi = psi_mem_avg10();
        if (psi >= PSI_MEM_URGENT_PCT   && tier < 2) tier = (tier < 1 ? 2 : tier + 1);
        else if (psi >= PSI_MEM_ESCALATE_PCT && tier < 3) tier++;
    }

    return tier;
}

static bool adaptive_kill_eligible(int tier, ProcInfo *p, MemInfo *mi, int zpct,
                                    bool ram_low, time_t now, int *hot_spared,
                                    int *dbg_win, long *dbg_age) {
    (void)ram_low;
    *dbg_win = -1;
    *dbg_age = -1;

    /* Never kill exempt (HAL/system/launcher) processes */
    if (p->rank == RANK_EXEMPT) return false;

    if (tier >= 2) {
        /* HIGH/DEEP may touch SEMI_PROTECTED if holding swap:
         * T3(DEEP): >=40MB swap; T2(HIGH): >=50MB swap */
        if (p->prio < PRIO_SEMI_PROTECTED) return false;
        long sp_thresh = (tier == 3) ? 40*1024 : 50*1024;
        if (p->prio == PRIO_SEMI_PROTECTED && p->swap_kb < sp_thresh) return false;
    } else {
        if (p->prio < PRIO_BACKGROUND) return false;
    }

    if (p->oom_adj <= ADJ_VISIBLE_MAX)                   return false;
    if (p->swap_kb < 256 && p->rss_kb < 64*1024)        return false;
    if (recently_killed(p->name))                        return false;
    if (p->raw_last_fg_do_not_read_directly > 0 &&
        (now - p->raw_last_fg_do_not_read_directly) < MIN_BG_KILL_AGE_S)           return false;

    /* T3(DEEP): critical RAM overrides retention for spared apps but never foreground (RANK_EXEMPT); a small RANK_HOT quota stays spared for light multitasking. */
    /* single shared definition, see ram_state_critical(). */
    bool ram_critical = ram_state_critical(mi);

    bool zram_critical = zpct >= ZRAM_CRIT_PCT;

    bool spare_this_hot = is_tier1_background(p) && (*hot_spared < T3_SPARE_HOT_N);

    bool retain_override = false;
    if (p->prio == PRIO_JUNK && !p->true_fg) {
        retain_override = true;
    } else if (tier == 3 && (ram_critical || zram_critical) &&
               !p->true_fg && !spare_this_hot) {
        if (ram_critical) {
            /* Real OOM proximity, not pool fullness: hard override, no gate. */
            retain_override = true;
        } else if (is_actively_retained(p->name)) {

            long freed = 0;
            if (try_squeeze_then_reclaim(p, &freed) && freed >= RECLAIM_MIN_FREED_KB) {
                logi("Adaptive[T3]: ZRAM critical — squeezed [%s] instead "
                     "of overriding (freed=%ldMB)", p->name, freed/1024);
            } else {
                retain_override = true;
            }
        } else {
            retain_override = true;
        }
    }
    if (is_actively_retained(p->name) && !retain_override) {
        if (spare_this_hot && tier == 3 && (ram_critical || zram_critical)) {
            (*hot_spared)++;
            logi("Adaptive[T3]: RAM critical — sparing HOT [%s] for "
                 "multitasking (%d/%d)", p->name, *hot_spared, T3_SPARE_HOT_N);
        }
        return false;
    }
    if (retain_override && is_actively_retained(p->name)) {
        if (p->prio == PRIO_JUNK)
            logw("Adaptive[T3]: confirmed runaway growth — overriding "
                 "retention for [%s]", p->name);
        else
            logw("Adaptive[T3]: %s (%d%%) — overriding retention for [%s]",
                 ram_critical ? "RAM critical" : "ZRAM critical",
                 ram_critical ? mi->avail_pct : zpct, p->name);
    }

    /* IDLE tiers scale against score_protect_window() (discounted per tier, IDLE-DEEP more) instead of flat age cutoffs, which let IDLE-DEEP sweep anything untouched for 20 s. */
    if (tier == -2 || tier == -3) {
        int hot_age = (tier == -3) ? IDLE_DEEP_HOT_AGE_S : IDLE_KILL_HOT_AGE_S;

        time_t rec = effective_fg_recency(p);

        if (is_never_kill_now(p)) return false;  /* fg/exempt: never */
        if (is_tier1_background(p)) {
            /* tier1-background apps: wait before idle-killing */
            if (rec == 0 || (now - rec) < hot_age) return false;
            *dbg_age = now - rec; *dbg_win = hot_age;
        } else {
            /* tier2/tier3: scaled score_protect_window instead of a flat age, so well-used apps stay protected during idle sweeps. */
            int pct = (tier == -3) ? IDLE_DEEP_PROTECT_PCT : IDLE_PROTECT_PCT;
            int win = score_protect_window(p->name) * pct / 100;
            if (rec > 0 && (now - rec) < win) return false;
            *dbg_age = rec > 0 ? now - rec : -1; *dbg_win = win;
        }
        /* cooldown still applies inside idle tiers */
    } else if (tier == -1) {
        /* MAINT: tier3 is always eligible; tier2 with significant swap only for the >= RANK_WARM_S idle half, so the frequency floor in app_rank() still protects heavy apps. */
        time_t maint_rec = effective_fg_recency(p);
        bool maint_eligible = (p->rank == RANK_TIER3) ||
                              (p->rank == RANK_TIER2 &&
                               (maint_rec == 0 || (now - maint_rec) >= RANK_WARM_S) &&
                               ram_low && p->swap_kb >= 80*1024);
        if (!maint_eligible) return false;
        /* No score protect window for MAINT stale apps */
    } else {

        int win = score_protect_window(p->name);

        time_t rec_t = effective_fg_recency(p);
        if (rec_t > 0 && (now - rec_t) < win) return false;
    }

    if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
        p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB &&
        p->prio != PRIO_JUNK)                             return false;
    if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
        p->swap_kb < 200*1024)                           return false;

    if (tier < 2 && tier != -3 && p->rank == RANK_TIER1) return false;
    if (tier == -3 && p->true_fg) return false;

    if (p->restart_count >= KILL_ROI_RC_THRESHOLD &&
        p->rss_kb < KILL_ROI_MIN_RSS_KB) {
        if (now - g_kill_roi_log_t >= LOG_RATELIMIT_S) {
            g_kill_roi_log_t = now;
            logi("KillROI: sparing [%s] rss=%ldMB rc=%d (below ROI floor)",
                 p->name, p->rss_kb/1024, p->restart_count);
        }
        return false;
    }

    return true;
}

static void adaptive_run_compaction(ZramInfo *z, int zpct, int kills, time_t now) {
    if (kills > 0) usleep(200000);

    MemInfo mi_chk;
    if (read_meminfo(&mi_chk) != 0) {
        meminfo_fail_note();
        return;
    }
    ZramInfo z_chk; zram_read_stats(&z_chk);
    if (mi_chk.avail_pct >= FREE_LOW_PCT && z_chk.used_pct < ZRAM_STUCK_PCT) {
        logi("Adaptive[T3]: kills alone relieved pressure "
             "(avail=%d%% ZRAM=%d%%) — skipping compaction stages",
             mi_chk.avail_pct, z_chk.used_pct);
    } else {
        int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
        if (fd >= 0) { write(fd, "3\n", 2); close(fd);
            logi("Adaptive[T3]: drop_caches=3 done"); }
        usleep(300000);

        fd = open("/proc/sys/vm/compact_memory", O_WRONLY);
        if (fd >= 0) { write(fd, "1\n", 2); close(fd);
            logi("Adaptive[T3]: compact_memory done"); }
        usleep(500000);

        zram_read_stats(&z_chk);
        if (z_chk.used_pct < ZRAM_TRIM_PCT) {
            logi("Adaptive[T3]: drop_caches+compact already sufficient "
                 "(ZRAM=%d%%) — skipping slow zram/compact stage",
                 z_chk.used_pct);
        } else if (g_zram_sys[0]) {
            char cpath[256];
            snprintf(cpath, sizeof(cpath), "%s/compact", g_zram_sys);
            fd = open(cpath, O_WRONLY);
            if (fd >= 0) {
                write(fd, "1\n", 2); close(fd);
                logi("Adaptive[T3]: zram/compact — waiting %dms",
                     ZRAM_COMPACT_WAIT_MS);
                usleep((useconds_t)ZRAM_COMPACT_WAIT_MS * 1000);
            } else {
                logi("Adaptive[T3]: zram/compact unavailable");
            }
        }
    }

    ZramInfo za; zram_read_stats(&za);
    int spct = (z->used_kb > 0)
               ? (int)((z->used_kb - za.used_kb) * 100 / z->used_kb) : 0;
    logi("Adaptive[T3]: done — %d%%→%d%% (%ldMB→%ldMB, recovered ~%d%%, killed=%d)",
         zpct, za.used_pct, z->used_kb/1024, za.used_kb/1024, spct, kills);

    g_last_deepclean   = now;
    g_zram_stuck_since = 0;

    /* Futility tracking */
    if (kills == 0 && spct <= DEEPCLEAN_FUTILE_PCT) {
        g_deepclean_fail_cnt++;
        if (g_deepclean_fail_cnt >= DEEPCLEAN_FUTILE_STRIKES) {
            logw("Adaptive[T3]: %d consecutive futile cleans — "
                 "pausing %d min",
                 g_deepclean_fail_cnt, DEEPCLEAN_FUTILE_PAUSE_S / 60);
            g_deepclean_pause_until = now + DEEPCLEAN_FUTILE_PAUSE_S;
            g_deepclean_fail_cnt    = 0;
        }
    } else {
        g_deepclean_fail_cnt = 0;
    }
}

static void adaptive_clean(ProcInfo *tbl, int cnt, MemInfo *mi, ZramInfo *z) {
    if (!z->active) return;

    int zpct    = z->used_pct;
    bool ram_low = mi->avail_pct < FREE_LOW_PCT;
    /* should_spare_kill() bypass is RAM-CRITICAL, not RAM-LOW: LOW-state adaptive kills (avail 8-15%) must not skip the squeeze/ROI gates only CRITICAL may bypass. */
    bool ram_critical = ram_state_critical(mi);
    time_t now  = time(NULL);

    bool do_compact = false, rising = false;
    int tier = adaptive_select_tier(mi, z, &do_compact, &rising);

    classify_dumpsys_drain();
    category_reconfirm_sweep();

    int max_kills, target_pct, intvl;
    switch (tier) {
    case 3:
        max_kills  = DEEPCLEAN_KILL_MAX;
        target_pct = DEEPCLEAN_KILL_TARGET_PCT;
        intvl      = ADAPTIVE_INTVL_HIGH_S;
        break;
    case 2:
        max_kills  = ADAPTIVE_KILLS_HIGH;
        target_pct = ADAPTIVE_TARGET_HIGH_PCT;
        intvl      = ADAPTIVE_INTVL_HIGH_S;
        break;
    case 1:
        max_kills  = ADAPTIVE_KILLS_MED;
        target_pct = ADAPTIVE_TARGET_MED_PCT;
        intvl      = ADAPTIVE_INTVL_MED_S;
        break;
    case 0:
        max_kills  = ADAPTIVE_KILLS_LOW;
        target_pct = ADAPTIVE_TARGET_LOW_PCT;
        intvl      = ADAPTIVE_INTVL_LOW_S;
        break;
    case -2: /* IDLE */
        max_kills  = IDLE_KILLS_MAX;
        target_pct = 0;
        intvl      = IDLE_INTVL_S;
        break;
    case -3: /* IDLE-DEEP: screen off 30+ min */
        max_kills  = IDLE_DEEP_KILLS_MAX;
        target_pct = IDLE_DEEP_TARGET_PCT;
        intvl      = IDLE_DEEP_INTVL_S;
        break;
    default: /* MAINTENANCE */
        max_kills  = ADAPTIVE_KILLS_MAINT;
        target_pct = 0;
        intvl      = ADAPTIVE_INTVL_MAINT_S;
        break;
    }

    /* Per-tier interval gate (7 independent timers: IDLE-DEEP/IDLE/MAINT/LOW/MED/HIGH/DEEP) */
    static time_t s_last_t[7] = {0, 0, 0, 0, 0, 0, 0};
    int tidx = tier + 3; /* 0=IDLE-DEEP,1=IDLE,2=MAINT,3=LOW,4=MED,5=HIGH,6=DEEP */
    if ((now - s_last_t[tidx]) < intvl) return;
    s_last_t[tidx] = now;

    long target_swap = target_pct > 0 ? z->disksize_kb * target_pct / 100 : 0;
    long freed_swap  = 0;
    int  kills       = 0;
    const char *tier_label = tier == 3  ? "T3(DEEP)" :
                             tier == 2  ? "T2" :
                             tier == 1  ? "T1" :
                             tier == 0  ? "T0" :
                             tier == -2 ? "IDLE" :
                             tier == -3 ? "IDLE-DEEP" : "MAINT";

    if (cnt > 1) qsort(tbl, (size_t)cnt, sizeof(ProcInfo), cmp_adaptive_clean);

    int hot_spared = 0; /* count of RANK_HOT apps spared from T3 critical override */
    for (int i = 0; i < cnt && kills < max_kills; i++) {
        if (target_pct > 0 && freed_swap >= target_swap) break;
        ProcInfo *p = &tbl[i];
        int   dbg_win;
        long  dbg_age;

        if (!adaptive_kill_eligible(tier, p, mi, zpct, ram_low, now,
                                     &hot_spared, &dbg_win, &dbg_age))
            continue;

        long adaptive_reclaimed = 0;
        if (should_spare_kill(p, ram_critical, &adaptive_reclaimed)) {
            freed_swap += adaptive_reclaimed;
            continue;
        }

        int krc = kill_proc_verified(p);
        if (krc == 0) {
            freed_swap += p->swap_kb + p->rss_kb / 4;
            kills++;
            record_kill(p);
            AppScore *as = score_lookup(p->name);
            if (as) { as->adaptive_kill_count++; as->dirty = true; g_scores_dirty = true; }

            logk("Adaptive[%s%s] rank%d pid=%-6d swap=%4ldMB rss=%4ldMB "
                 "win=%d age=%ld [%s]",
                 tier_label, rising ? " rising" : "",
                 p->rank, p->pid, p->swap_kb/1024, p->rss_kb/1024,
                 dbg_win, dbg_age, p->name);
            usleep(30000);
        }
    }

    if (kills > 0 || tier >= 2) {
        if (kills > 0)
            logi("Adaptive[%s]: killed %d proc(s), freed ~%ldMB "
                 "(avail=%d%% ZRAM=%d%% rising=%s)",
                 tier_label, kills, freed_swap/1024,
                 mi->avail_pct, zpct, rising ? "yes" : "no");
        if (!do_compact) {
            int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (fd >= 0) { write(fd, tier <= 0 ? "1\n" : "3\n", 2); close(fd); }
        }
    }

    if (do_compact) adaptive_run_compaction(z, zpct, kills, now);
}

/* Main daemon loop */
static void signal_handler(int sig) {
    g_shutdown_signal = sig;
    g_running = 0;
}

static void log_startup_banner(void) {
    logi("══════════════════════════════════════");
    logi("  LMK Engine %s by %s", LMK_VERSION, LMK_AUTHOR);
    logi("  ZRAM target %d%% of RAM", ZRAM_SIZE_PCT);
    logi("  ZRAM stages: warn>%d%% trim>%d%% stuck>%d%% crit>%d%%",
         ZRAM_WARN_PCT, ZRAM_TRIM_PCT, ZRAM_STUCK_PCT, ZRAM_CRIT_PCT);

    logi("  Adaptive clean: IDLE-DEEP/IDLE/MAINT/T0/T1/T2/T3(DEEP) always-running, 7 tiers");
    logi("  Adaptive intervals: %d/%d/%d/%d/%d/%d/%ds (IDLE-DEEP/IDLE/MAINT/LOW/MED/HIGH/DEEP)",
         IDLE_DEEP_INTVL_S, IDLE_INTVL_S, ADAPTIVE_INTVL_MAINT_S, ADAPTIVE_INTVL_LOW_S,
         ADAPTIVE_INTVL_MED_S, ADAPTIVE_INTVL_HIGH_S, ADAPTIVE_INTVL_HIGH_S);
    logi("  Adaptive kills: %d/%d/%d/%d/%d/%d/%d (IDLE-DEEP/IDLE/MAINT/LOW/MED/HIGH/DEEP)",
         IDLE_DEEP_KILLS_MAX, IDLE_KILLS_MAX, ADAPTIVE_KILLS_MAINT, ADAPTIVE_KILLS_LOW,
         ADAPTIVE_KILLS_MED, ADAPTIVE_KILLS_HIGH, DEEPCLEAN_KILL_MAX);
    logi("  T3(DEEP): fires when ZRAM stuck >%d%% for >%ds, cd=%ds; "
         "kills+drop_caches+compact_memory+zram/compact",
         ZRAM_STUCK_PCT, ZRAM_STUCK_S, ZRAM_DEEPCLEAN_CD_S);
    logi("  T3 futility: pause %dm after %d×≤%d%% recovery",
         DEEPCLEAN_FUTILE_PAUSE_S/60, DEEPCLEAN_FUTILE_STRIKES, DEEPCLEAN_FUTILE_PCT);
    logi("  ZRAM emerg kill: cd relaxed to %ds when all blocked at >%d%%",
         ZRAM_EMERG_CD_S, ZRAM_STUCK_PCT);
    logi("  Idle mode: COLD+STALE sweep after %ds no fg-change, every %ds, max %d kills",
         IDLE_DETECT_S, IDLE_INTVL_S, IDLE_KILLS_MAX);
    logi("  Rank cache: %s saved every %ds (avg_swap warm-up after restart)",
         RANK_CACHE_FILE, RANK_CACHE_SAVE_S);
    logi("  App ranking: 0=exempt 1=tier1(<%ds) 2=tier2(<%dmin) "
         "3=tier3 4=tier4/background",
         RANK_HOT_S, RANK_COLD_S/60);

    logi("  Proactive launch squeeze: target %d%% (heavy app %d%%) on fg app "
         "switch — cgroup memory.high squeeze first, do_kill() fallback "
         "only for any shortfall",
         LAUNCH_TRIM_TARGET_PCT, LAUNCH_TRIM_HEAVY_TARGET_PCT);
    logi("  RAM kill thresholds: low<%d%% crit<%d%% stop>%d%%",
         FREE_LOW_PCT, FREE_CRIT_PCT, FREE_HIGH_PCT);
    logi("  Protect window: %d–%ds (score+RAM-aware)",
         FG_PROTECT_BASE, FG_PROTECT_MAX);
    logi("  Kill cooldown: %d–%ds (restart_count scaled)",
         RESTART_CD_BASE_S, RESTART_CD_MAX_S);

    logi("  Retention: Phase A up to %d TIER1-only pin slot(s) (of %d TIER1 cap), "
         "Phase B up to %d shared slot(s) (of %d TIER2 cap) — real cgroup "
         "memory.low floors, held >=%lds after leaving foreground, "
         "candidacy requires use within %lds (%ds settle delay)",
         TIER1_PIN_MAX, TIER1_MAX_SLOTS, TIER2_PIN_MAX, TIER2_MAX_SLOTS,
         (long)TIER1_PIN_HOLD_S, (long)RETAIN_FG_AGE_S, PIN_SETTLE_S);
    logi("  Bounce suppression: rc>=%d and swap<%dMB, plus time-window (%d kills in %ds)",
         BOUNCE_SUPPRESS_RC, BOUNCE_SUPPRESS_SWAP_KB/1024,
         BOUNCE_WINDOW_KILLS, BOUNCE_WINDOW_S);
    logi("  Retention protect: pinned apps exempt from opportunistic kills for %ds after pin",
         RETAIN_PROTECT_GRACE_S);
    logi("  Log rate-limit: noisy-state messages throttled to 1/%ds",
         LOG_RATELIMIT_S);
    logi("══════════════════════════════════════");
}

/* Sequential init: priority/name, start-time file, persisted state, PSI setup, ZRAM discovery and setup. */
static void daemon_init(void) {
    setpriority(PRIO_PROCESS, 0, -15);
    prctl(PR_SET_NAME, "lmk_engine", 0, 0, 0);

    g_start_time = time(NULL);
    {
        FILE *sf = fopen(START_TIME_FILE, "w");
        if (sf) { fprintf(sf, "%ld\n", (long)g_start_time); fclose(sf); }
    }

    score_load(false);
    rank_cache_load();
    psi_check_available();
    psi_monitor_init();
    lmkd_minfree_cleanup();

    if (zram_find(g_zram_dev, sizeof(g_zram_dev), g_zram_sys, sizeof(g_zram_sys))) {
        zram_setup();
    } else {
        logw("ZRAM: no device found – running without ZRAM management");
    }

    pin_kswapd_to_little_cores();
}

/* Wait for sys.boot_completed up to BOOT_WAIT_S so we don't act on an incomplete process table or a missing appwidgets.xml. */
#define BOOT_WAIT_S  90

#define FRESH_BOOT_UPTIME_MAX_S  300
static double read_uptime_s(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1.0;
    double up = -1.0;
    if (fscanf(f, "%lf", &up) != 1) up = -1.0;
    fclose(f);
    return up;
}
static void wait_for_boot_settle(void) {

    double up = read_uptime_s();
    if (up >= 0.0 && up > FRESH_BOOT_UPTIME_MAX_S) {
        logi("Boot: system uptime %.0fs — manual restart, not a fresh boot; "
             "skipping widget-settle grace", up);
        g_widget_settle_until = 0;
        return;
    }

    logi("Boot: waiting for sys.boot_completed (max %ds)", BOOT_WAIT_S);
    time_t deadline = time(NULL) + BOOT_WAIT_S;
    bool settled = false;
    while (!settled && g_running && time(NULL) < deadline) {
        FILE *gp = popen("getprop sys.boot_completed 2>/dev/null", "r");
        if (gp) {
            char val[8] = {0};
            fgets(val, sizeof(val), gp);
            pclose(gp);
            if (val[0] == '1') { settled = true; }
        }
        if (!settled) sleep(2);
    }
    if (settled)
        logi("Boot: settled – entering main loop");
    else
        logw("Boot: timeout after %ds – continuing anyway", BOOT_WAIT_S);
    /* Allow AppWidgetService extra time to write appwidgets.xml */
    g_widget_settle_until = time(NULL) + WIDGET_SETTLE_S;
    logi("Boot: widget-settle grace %ds (kills suppressed until providers loaded)", WIDGET_SETTLE_S);
}

/* Screen-off or Doze fast-tracks the COLD+STALE idle sweep timer, since there's no fg change to wait for. */
static void update_idle_detection_state(time_t now) {
    if (g_last_fg_change_t == 0) g_last_fg_change_t = now;
    bool scr_off = is_screen_off();
    if (scr_off) {
        if (g_screen_off_since == 0) g_screen_off_since = now;
    } else if (g_screen_off_since != 0) {
        logi("Idle: screen on — exiting idle-deep window (was off %ldm)",
             (long)(now - g_screen_off_since) / 60);
        g_screen_off_since = 0;

        drop_clean_caches("screen-on/unlock");
    }
    if ((scr_off || is_doze_active()) &&
        (now - g_last_fg_change_t) < IDLE_DETECT_S)
        g_last_fg_change_t = now - IDLE_DETECT_S; /* fast-track idle */
}

/* True-foreground detection + debounce + proactive trim; skipped during the widget-settle grace window. */
static void handle_true_foreground_detection(ProcInfo *tbl, int cnt,
                                              MemInfo *mi, ZramInfo *z) {
    time_t now = time(NULL);
    if (now < g_widget_settle_until) {
        /* still in widget-settle grace, skip kills */
        return;
    }
    char fg_name[256] = {0};
    bool found_fg = find_true_foreground(tbl, cnt, fg_name, sizeof(fg_name));
    if (found_fg && strcmp(fg_name, g_true_fg_name) != 0) {
        /* Debounce: require the same candidate across consecutive ticks to avoid scan-order flicker when several procs share ADJ_FOREGROUND. */
        if (strcmp(fg_name, g_pending_fg_name) == 0) {
            g_pending_fg_streak++;
        } else {
            strncpy(g_pending_fg_name, fg_name, sizeof(g_pending_fg_name) - 1);
            g_pending_fg_name[sizeof(g_pending_fg_name) - 1] = '\0';
            g_pending_fg_streak = 1;
        }
        if (g_pending_fg_streak >= FG_DEBOUNCE_TICKS) {

            char base_pkg[128];
            strncpy(base_pkg, fg_name, sizeof(base_pkg) - 1);
            base_pkg[sizeof(base_pkg) - 1] = '\0';
            char *colon = strchr(base_pkg, ':');
            if (colon) *colon = '\0';
            bool confirmed = is_launcher_like(fg_name) ||
                              confirm_true_foreground(base_pkg);
            if (!confirmed) {
                logi("Launch: %s reached fg (adj=0) but isn't the "
                     "focused activity — skipping trim/credit "
                     "(likely overlay/foreground-service)", fg_name);
            } else {
                if (!is_launcher_like(fg_name))
                    proactive_squeeze(tbl, cnt, mi, z, fg_name);
                strncpy(g_true_fg_name, fg_name, sizeof(g_true_fg_name) - 1);
                g_true_fg_name[sizeof(g_true_fg_name) - 1] = '\0';
                g_last_fg_change_t = now; /* reset idle timer on fg switch */
            }
            g_pending_fg_streak = 0;
            g_pending_fg_name[0] = '\0';
        }
    } else if (found_fg && strcmp(fg_name, g_true_fg_name) == 0) {
        /* Candidate matches confirmed fg again, clear stale pending state */
        g_pending_fg_streak = 0;
        g_pending_fg_name[0] = '\0';
    }
}

/* ZRAM stage management: drop_caches nudge, pressure-kill escalation, stuck tracking and the critical-band cycle; last_zram_cycle/last_drop_caches are caller-owned. */
static void handle_zram_stage_management(ProcInfo *tbl, int cnt,
                                          MemInfo *mi, ZramInfo *z, time_t now,
                                          time_t *last_zram_cycle,
                                          time_t *last_drop_caches) {
    if (!z->active) return;

    if (g_zram_needs_resize)
        zram_try_deferred_resize(mi);

    if (z->used_pct >= ZRAM_WARN_PCT && z->used_pct < ZRAM_TRIM_PCT) {
        if (now - *last_drop_caches > 60) {
            *last_drop_caches = now;
            int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
        }
    }

    if (z->used_pct >= ZRAM_TRIM_PCT) {
        if (time(NULL) >= g_widget_settle_until)
        zram_pressure_kill(tbl, cnt, z->used_pct);
    }

    /* Stuck ZRAM tracking, Adaptive[T3] fires automatically */
    {
        if (z->used_pct >= ZRAM_STUCK_PCT) {
            if (g_zram_stuck_since == 0) {
                g_zram_stuck_since = now;
                logi("ZRAMstuck: ZRAM at %d%% — Adaptive[T3] will engage "
                     "after %ds",
                     z->used_pct, ZRAM_STUCK_S);
            } else if (g_deepclean_pause_until > 0 &&
                       now < g_deepclean_pause_until) {
                if (now - g_log_stuck_t >= LOG_RATELIMIT_S) {
                    g_log_stuck_t = now;
                    logi("ZRAMstuck: T3 paused after futile runs (%ldm remain)",
                         (long)(g_deepclean_pause_until - now) / 60);
                }
            } else {
                if (now - g_log_stuck_t >= LOG_RATELIMIT_S) {
                    g_log_stuck_t = now;
                    long cd_remain = (g_last_deepclean == 0) ? 0 :
                        (long)(ZRAM_DEEPCLEAN_CD_S - (now - g_last_deepclean));
                    if (cd_remain < 0) cd_remain = 0;
                    logi("ZRAMstuck: %d%% for %lds — T3 eligible=%s cd_remain=%lds",
                         z->used_pct, (long)(now - g_zram_stuck_since),
                         (now - g_zram_stuck_since) >= ZRAM_STUCK_S ? "yes" : "no",
                         cd_remain);
                }
            }
        } else {
            if (g_zram_stuck_since != 0) {
                logi("ZRAMstuck: cleared (now %d%%)", z->used_pct);
                g_zram_stuck_since = 0;
                g_log_stuck_t      = 0;
            }
        }
    }

    if (z->used_pct >= ZRAM_CRIT_PCT) {
        if ((now - *last_zram_cycle) > 300) {
            long need_kb = z->orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
            bool can_cycle = (mi->avail_kb >= need_kb);
            if (can_cycle) {
                *last_zram_cycle = now;
                /* Outcome is logged inside; g_zram_active is already re-synced before zram_cycle() returns. */
                (void)zram_cycle(false);
            } else {
                *last_zram_cycle = now;
                if (now - g_log_crit_t >= LOG_RATELIMIT_S) {
                    g_log_crit_t = now;
                    logi("ZRAMcrit: cannot cycle passively (orig=%ldMB avail=%ldMB) "
                         "— forced pre-clean disabled, see comment above",
                         z->orig_data_kb/1024, mi->avail_kb/1024);
                }
            }
        }
    }
}

/* RAM-pressure kill pass + adaptive loop period; the poll interval is updated before any early return so idle periods use the slower, cheaper cadence. */
static void ram_pressure_and_adapt(ProcInfo *tbl, int cnt,
                                    MemInfo *mi, ZramInfo *z, time_t now,
                                    useconds_t *loop_sleep_us) {
    bool calm = (mi->avail_pct >= FREE_LOW_PCT + 5) &&
                (z->used_pct < ZRAM_WARN_PCT) &&
                (now - g_last_fg_change_t) > 5;
    *loop_sleep_us = calm ? 1500000 : 500000;

    int avail = mi->avail_pct;
    if (avail >= FREE_HIGH_PCT) return;

    /* shared definition, see ram_state_critical(). */
    int cd = ram_state_critical(mi) ? 1 : 5;
    if ((now - g_last_kill_t) < cd) return;

    long want, freed = 0;
    if (ram_state_critical(mi)) {
        want = (long)g_total_ram_kb * FREE_LOW_PCT / 100 - mi->avail_kb;
        if (want < 64*1024) want = 64*1024;
        freed = do_kill(tbl, cnt, want, PRIO_SEMI_PROTECTED, true, avail);
        logi("CRITICAL: freed %ldMB", freed/1024);
    } else if (avail < FREE_LOW_PCT) {
        want = (long)g_total_ram_kb * (FREE_LOW_PCT + 4) / 100 - mi->avail_kb;
        if (want < 32*1024) want = 32*1024;
        freed = do_kill(tbl, cnt, want, PRIO_BACKGROUND, false, avail);
        if (freed > 0) logi("LOW: freed %ldMB", freed/1024);
    }

    g_last_kill_t = now;
    if (freed == 0 && avail < FREE_CRIT_PCT) {
        logi("Nothing killable – backing off 5s");
        g_last_kill_t += 5;
    }

}

/* LMKD PSI tuning is opt-in (persist.lmk.psitune=1 or psitune=1 in /data/adb/lmk_engine.conf): forcing 200ms and restarting lmkd changes system kill behavior on every boot. */
#define LMK_CONF_FILE "/data/adb/lmk_engine.conf"

static bool config_flag_enabled(const char *key) {
    /* Config file first (simple key=1 lines), then the system property. */
    FILE *f = fopen(LMK_CONF_FILE, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (strcmp(line, key) == 0 && strcmp(eq + 1, "1") == 0) {
                fclose(f);
                return true;
            }
        }
        fclose(f);
    }
    char cmd[160], val[32] = {0};
    snprintf(cmd, sizeof(cmd),
             "getprop persist.lmk.%s 2>/dev/null", key);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (!fgets(val, sizeof(val), fp)) val[0] = 0;
        pclose(fp);
    }
    int len = (int)strlen(val);
    while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r')) val[--len] = 0;
    return strcmp(val, "1") == 0;
}

static void fix_lmkd_psi_stall(void) {
    if (!config_flag_enabled("psitune")) {
        logi("PSItune: disabled (opt-in) — enable with persist.lmk.psitune=1 "
             "or psitune=1 in %s", LMK_CONF_FILE);
        return;
    }
    FILE *fp = popen("getprop ro.lmk.psi_partial_stall_ms 2>/dev/null", "r");
    char val[32] = {0};
    if (fp) { if (!fgets(val, sizeof(val), fp)) val[0] = 0; pclose(fp); }
    int len = (int)strlen(val);
    while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r')) val[--len] = 0;

    if (strcmp(val, "200") == 0) {
        logi("PSItune: ro.lmk.psi_partial_stall_ms already 200ms, no action");
        return;
    }
    logi("PSItune: ro.lmk.psi_partial_stall_ms=%s (expected 200ms low-ram "
         "profile) — correcting and restarting lmkd to apply this boot "
         "(opt-in via persist.lmk.psitune=1)",
         val[0] ? val : "(unset/default 70ms high-end profile)");
    if (system("resetprop ro.lmk.psi_partial_stall_ms 200 2>/dev/null") != 0)
        logw("PSItune: resetprop call failed — is Magisk's resetprop on PATH?");
    /* Check stop and start separately so a half-failed restart is visible in the log; never blocks daemon startup. */
    if (system("stop lmkd 2>/dev/null") != 0)
        logw("PSItune: 'stop lmkd' failed — value set but not applied yet");
    if (system("start lmkd 2>/dev/null") != 0)
        logw("PSItune: 'start lmkd' failed — lmkd may be down until next "
             "reboot; check 'getprop init.svc.lmkd'");
}

static void run_daemon(void) {
    log_startup_banner();
    daemon_init();
    fix_lmkd_psi_stall();
    wait_for_boot_settle();

    ProcInfo *tbl = malloc(2048 * sizeof(ProcInfo));
    if (!tbl) {
        loge("malloc failed");
        logcat_watch_stop(&g_logcat_sys);
        logcat_watch_stop(&g_logcat_events);
        return;
    }

    MemInfo mi;
    int  tick = 0;
    time_t last_zram_cycle  = 0;
    time_t last_drop_caches = 0;
    useconds_t loop_sleep_us = 500000; /* adaptive: 500ms busy, up to 1.5s calm */

    double prof_enum_ms = 0, prof_pin_ms = 0, prof_adapt_ms = 0, prof_zram_ms = 0;

    while (g_running) {
        wait_for_next_tick(loop_sleep_us);
        if (read_meminfo(&mi) < 0) { meminfo_fail_note(); continue; }
        g_meminfo_fail_streak = 0;
        load_active_widget_pkgs();

        struct timespec _t0, _t1;
        clock_gettime(CLOCK_MONOTONIC, &_t0);
        int cnt = enumerate_procs(tbl, 2048);
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        prof_enum_ms += ts_diff_ms(&_t0, &_t1);
        if (cnt < 0) continue;

        track_foreground(tbl, cnt);
        logcat_kill_watch(tbl, cnt); /* External-kill attribution, diagnostic only */
        score_maybe_save();
        rank_cache_maybe_save(tbl, cnt);

        /* Active retention must run before the kill paths so is_actively_retained() is current. */
        clock_gettime(CLOCK_MONOTONIC, &_t0);
        oom_pin_retained(tbl, cnt, mi.avail_pct);
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        prof_pin_ms += ts_diff_ms(&_t0, &_t1);

        ZramInfo z; zram_read_stats(&z);
        g_zram_used_pct = z.used_pct;

        apply_avail_tier_tunables(mi.avail_pct, g_zram_used_pct);

        pin_squeeze_pass(tbl, cnt);
        if (++tick >= 20) {
            tick = 0;
            double self_cpu_pct = 0.0, kswapd_cpu_pct = 0.0;
            cpu_track_sample(&self_cpu_pct, &kswapd_cpu_pct);
            logi("RAM total=%ldMB avail=%ldMB(%d%%)  "
                 "ZRAM %ldMB/%ldMB(%d%%)  orig=%ldMB  procs=%d  "
                 "cpu self=%.1f%% kswapd=%.1f%%  "
                 "state=%s",
                 mi.total_kb/1024, mi.avail_kb/1024, mi.avail_pct,
                 z.used_kb/1024, z.disksize_kb/1024, z.used_pct,
                 z.orig_data_kb/1024, cnt, self_cpu_pct, kswapd_cpu_pct,
                 mi.avail_pct < FREE_CRIT_PCT ? "CRITICAL" :
                 (mi.avail_pct < FREE_LOW_PCT ? "LOW" : "NORMAL"));
            logi("Profile: enum=%.1fms pin=%.1fms adapt=%.1fms zram=%.1fms "
                 "(avg ms/tick over last 20)",
                 prof_enum_ms/20, prof_pin_ms/20, prof_adapt_ms/20, prof_zram_ms/20);
            prof_enum_ms = prof_pin_ms = prof_adapt_ms = prof_zram_ms = 0;
        }

        if (z.active) update_swappiness(z.used_pct);

        time_t now = time(NULL);

        unattrib_check_vanished(tbl, cnt, now);
        unattrib_mark_seen(tbl, cnt, now);

        update_idle_detection_state(now);
        handle_true_foreground_detection(tbl, cnt, &mi, &z);

        /* Full reset sweep (self-gated: 12h + 30min idle) */
        if (time(NULL) >= g_widget_settle_until)
        full_reset_sweep(tbl, cnt);

        if (time(NULL) >= g_widget_settle_until) {
            clock_gettime(CLOCK_MONOTONIC, &_t0);
            adaptive_clean(tbl, cnt, &mi, &z);
            clock_gettime(CLOCK_MONOTONIC, &_t1);
            prof_adapt_ms += ts_diff_ms(&_t0, &_t1);
        }

        clock_gettime(CLOCK_MONOTONIC, &_t0);
        handle_zram_stage_management(tbl, cnt, &mi, &z, now,
                                      &last_zram_cycle, &last_drop_caches);
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        prof_zram_ms += ts_diff_ms(&_t0, &_t1);

        ram_pressure_and_adapt(tbl, cnt, &mi, &z, now, &loop_sleep_us);
    }

    if (g_shutdown_signal)
        logi("Signal %d – shutting down", (int)g_shutdown_signal);
    free(tbl);
    score_save();
    /* SIGTERM -> bounded wait -> SIGKILL: never block in waitpid() on a
     * logcat that has nothing new to write (the old pclose() hang). */
    logcat_watch_stop(&g_logcat_sys);
    logcat_watch_stop(&g_logcat_events);
    logi("LMK Engine stopped.");
}

/* PID file is "<pid> <starttime>\n" (stat field 22), so a recycled PID is told apart; malformed content never triggers action against any process.
 * A legacy "<pid>" file is trusted only if /proc/<pid>/cmdline looks like "...lmk_engine... --start"; legacy daemons hold no lock, so --start refuses while one lives.
 * Singleton: flock(LOCK_EX|LOCK_NB) on LOCK_FILE_PATH, taken before any status check and held for the daemon's lifetime; the fd is inherited across fork() (the parent only closes its copy, never LOCK_UN)
 * and is O_CLOEXEC so helpers can't pin it. The lock file is never unlinked: that would create two inodes, hence two locks. */
#define PID_FILE        "/data/local/tmp/lmk_engine.pid"
#define LOCK_FILE_PATH  "/data/local/tmp/lmk_engine.lock"
#define PID_MAX_SANE    4194304L          /* PID_MAX_LIMIT on 64-bit Linux */
#define STOP_WAIT_MS    10000             /* graceful-shutdown budget       */
#define START_CONFIRM_MS 10000            /* parent waits for child "ready" */

typedef enum { PM_MISSING, PM_MALFORMED, PM_LEGACY, PM_VALID, PM_ERROR } PidMetaKind;
typedef struct {
    PidMetaKind kind;
    pid_t pid;
    unsigned long long starttime;
    int err;                      /* errno for PM_ERROR */
} PidMeta;

static PidMeta pid_meta_read(void) {
    PidMeta m; memset(&m, 0, sizeof(m));
    int fd = open(PID_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) { m.kind = PM_MISSING; return m; }
        m.kind = PM_ERROR; m.err = errno; return m;
    }
    char buf[64];
    ssize_t n;
    do { n = read(fd, buf, sizeof(buf) - 1); } while (n < 0 && errno == EINTR);
    int rerr = errno;
    close(fd);
    if (n < 0) { m.kind = PM_ERROR; m.err = rerr; return m; }
    if (n == 0 || n >= (ssize_t)sizeof(buf) - 1) { m.kind = PM_MALFORMED; return m; }
    buf[n] = '\0';

    char *e = NULL;
    errno = 0;
    long pv = strtol(buf, &e, 10);
    if (e == buf || errno != 0 || pv <= 1 || pv > PID_MAX_SANE) {
        m.kind = PM_MALFORMED; return m;
    }
    if (*e == '\0' || (*e == '\n' && e[1] == '\0')) {   /* legacy: pid only */
        m.kind = PM_LEGACY; m.pid = (pid_t)pv; return m;
    }
    if (*e != ' ' || !isdigit((unsigned char)e[1])) { m.kind = PM_MALFORMED; return m; }
    char *e2 = NULL;
    errno = 0;
    unsigned long long st = strtoull(e + 1, &e2, 10);
    if (errno != 0 || st == 0 || !(*e2 == '\0' || (*e2 == '\n' && e2[1] == '\0'))) {
        m.kind = PM_MALFORMED; return m;
    }
    m.kind = PM_VALID; m.pid = (pid_t)pv; m.starttime = st;
    return m;
}

static bool pid_meta_same(const PidMeta *a, const PidMeta *b) {
    return a->kind == b->kind && a->pid == b->pid && a->starttime == b->starttime;
}

typedef enum { ID_MATCH, ID_GONE, ID_MISMATCH, ID_UNKNOWN } IdResult;

/* starttime with error classification. 0 ok; -1 gone (ENOENT/ESRCH or
 * zombie/dead state); otherwise -errno-like >0 code in *err. */
static int proc_starttime_ex(pid_t pid, unsigned long long *st, int *err) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT || errno == ESRCH) return -1;
        *err = errno; return 1;
    }
    char buf[1024];
    char *got = fgets(buf, sizeof(buf), f);
    int ferr = ferror(f) ? errno : 0;
    fclose(f);
    if (!got) {
        if (ferr == ESRCH) return -1;          /* process vanished mid-read */
        *err = ferr ? ferr : EIO; return 1;
    }
    char *p = strrchr(buf, ')');               /* comm may contain spaces/')' */
    if (!p || p[1] != ' ') { *err = EPROTO; return 1; }
    char state = p[2];
    if (state == 'Z' || state == 'X' || state == 'x') return -1;   /* dead */
    unsigned long long v = 0;
    if (sscanf(p + 2,
        "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
        &v) != 1 || v == 0) { *err = EPROTO; return 1; }
    *st = v;
    return 0;
}

/* Legacy heuristic: cmdline == "<...lmk_engine...>\0--start\0". */
static IdResult legacy_cmdline_check(pid_t pid, int *err) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ESRCH) return ID_GONE;
        *err = errno; return ID_UNKNOWN;
    }
    char buf[512];
    ssize_t n;
    do { n = read(fd, buf, sizeof(buf) - 1); } while (n < 0 && errno == EINTR);
    int rerr = errno;
    close(fd);
    if (n < 0) {
        if (rerr == ESRCH) return ID_GONE;
        *err = rerr; return ID_UNKNOWN;
    }
    if (n == 0) return ID_MISMATCH;            /* kernel thread / zombie */
    buf[n] = '\0';
    const char *a0 = buf;
    size_t l0 = strlen(a0);
    const char *a1 = (l0 + 1 < (size_t)n) ? a0 + l0 + 1 : "";
    const char *base = strrchr(a0, '/'); base = base ? base + 1 : a0;
    return (strstr(base, "lmk_engine") && !strcmp(a1, "--start")) ? ID_MATCH : ID_MISMATCH;
}

/* Does `pid` still carry the identity recorded in `m`? */
static IdResult meta_identity_check(const PidMeta *m, int *err) {
    if (m->kind == PM_LEGACY) return legacy_cmdline_check(m->pid, err);
    unsigned long long cur = 0;
    int rc = proc_starttime_ex(m->pid, &cur, err);
    if (rc < 0) return ID_GONE;
    if (rc > 0) return ID_UNKNOWN;
    return cur == m->starttime ? ID_MATCH : ID_MISMATCH;
}

/* singleton lock */
typedef enum { LK_OK, LK_BUSY, LK_ERR } LockResult;

static LockResult singleton_lock_try(int *fd_out, int *err) {
    *fd_out = -1;
    int fd = open(LOCK_FILE_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0 && (errno == EACCES || errno == EROFS))   /* flock works on RO fds */
        fd = open(LOCK_FILE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *err = errno; return LK_ERR; }
    int rc;
    do { rc = flock(fd, LOCK_EX | LOCK_NB); } while (rc < 0 && errno == EINTR);
    if (rc == 0) { *fd_out = fd; return LK_OK; }
    int e = errno;
    close(fd);
    if (e == EWOULDBLOCK) return LK_BUSY;
    *err = e; return LK_ERR;
}

/* A concurrent status/stop probe (or another --start) may hold the lock
 * for microseconds; retry briefly before concluding that a real daemon
 * owns it. */
static LockResult singleton_lock_acquire(int *fd_out, int *err, int retry_ms) {
    LockResult r = singleton_lock_try(fd_out, err);
    for (int waited = 0; r == LK_BUSY && waited < retry_ms; waited += 50) {
        usleep(50000);
        r = singleton_lock_try(fd_out, err);
    }
    return r;
}

/* 1 = held by someone else, 0 = free, -1 = cannot tell. Never creates
 * the lock file. */
static int singleton_lock_probe(int *err) {
    int fd = open(LOCK_FILE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        *err = errno; return -1;
    }
    int res, rc;
    do { rc = flock(fd, LOCK_SH | LOCK_NB); } while (rc < 0 && errno == EINTR);
    if (rc == 0)                 res = 0;
    else if (errno == EWOULDBLOCK) res = 1;
    else { *err = errno;        res = -1; }
    close(fd);                   /* drops our shared probe lock */
    return res;
}

/* daemon state probing */
typedef enum { DS_STOPPED, DS_RUNNING, DS_UNKNOWN } DaemonState;
typedef struct {
    DaemonState state;
    PidMeta     meta;             /* what the PID file said (if anything) */
    bool        stale_file;       /* PID file present but proves nothing alive */
    char        why[160];
} DaemonProbe;

/* have_lock: the caller already owns the singleton lock, so lock
 * probing (which would collide with our own lock) is skipped. */
static void daemon_probe(DaemonProbe *out, bool have_lock) {
    memset(out, 0, sizeof(*out));
    out->meta = pid_meta_read();
    const PidMeta *m = &out->meta;
    int err = 0;

    switch (m->kind) {
    case PM_VALID:
    case PM_LEGACY: {
        IdResult id = meta_identity_check(m, &err);
        if (id == ID_MATCH) {
            out->state = DS_RUNNING;
            snprintf(out->why, sizeof(out->why), "%s",
                     m->kind == PM_LEGACY ? "legacy pid file (identity by cmdline)"
                                          : "pid+starttime verified");
        } else if (id == ID_UNKNOWN) {
            out->state = DS_UNKNOWN;
            snprintf(out->why, sizeof(out->why),
                     "cannot verify pid %d: %s", (int)m->pid, strerror(err));
        } else {
            out->state = DS_STOPPED; out->stale_file = true;
            snprintf(out->why, sizeof(out->why),
                     "stale pid file (pid %d %s)", (int)m->pid,
                     id == ID_GONE ? "no longer exists" : "now belongs to another process");
        }
        return;
    }
    case PM_ERROR:
        out->state = DS_UNKNOWN;
        snprintf(out->why, sizeof(out->why), "cannot read %s: %s",
                 PID_FILE, strerror(m->err));
        return;
    case PM_MISSING:
    case PM_MALFORMED:
        break;
    }

    /* No usable PID metadata: only the lock can tell us anything. */
    const char *what = (m->kind == PM_MALFORMED) ? "pid file malformed/empty"
                                                 : "no pid file";
    if (have_lock) {
        out->state = DS_STOPPED;
        out->stale_file = (m->kind == PM_MALFORMED);
        snprintf(out->why, sizeof(out->why), "%s", what);
        return;
    }
    int lk = singleton_lock_probe(&err);
    if (lk == 1) {
        out->state = DS_UNKNOWN;
        snprintf(out->why, sizeof(out->why),
                 "singleton lock held but %s (instance starting, or pid file lost)", what);
    } else if (lk < 0) {
        out->state = DS_UNKNOWN;
        snprintf(out->why, sizeof(out->why),
                 "%s; lock probe failed: %s", what, strerror(err));
    } else {
        out->state = DS_STOPPED;
        out->stale_file = (m->kind == PM_MALFORMED);
        snprintf(out->why, sizeof(out->why), "%s", what);
    }
}

/* Remove a PID file that proves nothing alive, but only while holding
 * the singleton lock (so no live daemon can be mid-write) and only if
 * its content is still exactly what the caller examined (never delete a
 * file that now belongs to a different instance). */
static void pid_file_remove_stale(const PidMeta *seen) {
    int lfd = -1, err = 0;
    if (singleton_lock_try(&lfd, &err) != LK_OK) return;   /* someone alive: leave it */
    PidMeta now = pid_meta_read();
    if (pid_meta_same(&now, seen) && (now.kind == PM_VALID || now.kind == PM_LEGACY ||
                                      now.kind == PM_MALFORMED)) {
        if (unlink(PID_FILE) != 0 && errno != ENOENT)
            fprintf(stderr, "warning: cannot remove stale %s: %s\n",
                    PID_FILE, strerror(errno));
    }
    close(lfd);
}

/* daemon-side file ownership */
static int g_lock_fd = -1;       /* singleton lock, held until process exit */

/* Atomically publish "<pid> <starttime>\n" (tmp + rename). Every stage
 * is checked; false means the daemon must NOT run without verifiable
 * identity metadata. */
static bool pid_file_write_mine(void) {
    unsigned long long st = 0; int err = 0;
    if (proc_starttime_ex(getpid(), &st, &err) != 0) return false;
    char tmp[sizeof(PID_FILE) + 24];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", PID_FILE, (int)getpid());
    char line[64];
    int len = snprintf(line, sizeof(line), "%d %llu\n", (int)getpid(), st);
    if (len <= 0 || (size_t)len >= sizeof(line)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    const char *q = line; size_t left = (size_t)len;
    while (left > 0) {
        ssize_t w = write(fd, q, left);
        if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
        q += w; left -= (size_t)w;
    }
    if (ok && fsync(fd) != 0 && errno != EINVAL && errno != EROFS) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, PID_FILE) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

/* Shutdown: delete the PID file only if it still describes THIS process. */
static void pid_file_remove_if_mine(void) {
    PidMeta m = pid_meta_read();
    unsigned long long st = 0; int err = 0;
    if (m.kind == PM_VALID && m.pid == getpid() &&
        proc_starttime_ex(getpid(), &st, &err) == 0 && st == m.starttime)
        unlink(PID_FILE);
    else if (m.kind == PM_LEGACY && m.pid == getpid())
        unlink(PID_FILE);
}

/* verified stop */
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_send_signal
#define __NR_pidfd_send_signal 424
#endif

/* Wait for the target to exit; pidfd (if any) becomes readable at exit. */
static bool daemon_wait_exit(int pidfd, const PidMeta *m, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; ) {
        if (pidfd >= 0) {
            struct pollfd pf = { .fd = pidfd, .events = POLLIN };
            int rc = poll(&pf, 1, 100);
            if (rc > 0) return true;
            if (rc < 0 && errno != EINTR) return false;
            waited += 100;
        } else {
            int err = 0;
            if (meta_identity_check(m, &err) != ID_MATCH) return true;
            usleep(100000); waited += 100;
        }
    }
    return false;
}

/* Exit codes: 0 stopped / not running, 1 failed or timed out,
 *             2 state could not be determined (permissions/errors). */
static int kill_daemon(void) {
    DaemonProbe pr; daemon_probe(&pr, false);

    if (pr.state == DS_STOPPED) {
        puts("LMK Engine: not running");
        if (pr.stale_file) pid_file_remove_stale(&pr.meta);
        return 0;
    }
    if (pr.state == DS_UNKNOWN) {
        fprintf(stderr, "LMK Engine: cannot determine daemon state: %s\n"
                        "No signal sent.\n", pr.why);
        return 2;
    }

    const PidMeta *m = &pr.meta;
    int err = 0;
    int pfd = (int)syscall(__NR_pidfd_open, m->pid, 0);
    bool use_pidfd = pfd >= 0;
    if (!use_pidfd) {
        if (errno == ESRCH) { puts("LMK Engine: not running"); return 0; }
        if (errno != ENOSYS) {
            /* EMFILE/ENFILE/EPERM/...: never degrade to the racy path. */
            fprintf(stderr, "pidfd_open(%d): %s — refusing to signal without "
                            "a pinned handle\n", (int)m->pid, strerror(errno));
            return 1;
        }
    }

    /* Identity is verified AFTER the pidfd pins the process, and again
     * implicitly by signalling through that same pidfd. */
    IdResult id = meta_identity_check(m, &err);
    if (id == ID_GONE || id == ID_MISMATCH) {
        if (use_pidfd) close(pfd);
        puts("LMK Engine: not running (pid file was stale)");
        pid_file_remove_stale(m);
        return 0;
    }
    if (id == ID_UNKNOWN) {
        if (use_pidfd) close(pfd);
        fprintf(stderr, "LMK Engine: cannot verify pid %d: %s\nNo signal sent.\n",
                (int)m->pid, strerror(err));
        return 2;
    }

    int rc;
    if (use_pidfd) {
        rc = (int)syscall(__NR_pidfd_send_signal, pfd, SIGTERM, NULL, 0);
        if (rc < 0 && errno == ENOSYS) { close(pfd); pfd = -1; use_pidfd = false; }
    }
    if (!use_pidfd) {
        /* Fallback (no pidfd support): re-verify immediately before kill();
         * a residual check-then-kill window is unavoidable here. */
        IdResult fid = meta_identity_check(m, &err);
        if (fid == ID_UNKNOWN) {
            fprintf(stderr, "LMK Engine: cannot verify pid %d: %s\nNo signal sent.\n",
                    (int)m->pid, strerror(err));
            return 2;
        }
        if (fid != ID_MATCH) {
            puts("LMK Engine: not running (identity changed)");
            return 0;
        }
        rc = kill(m->pid, SIGTERM);
    }
    if (rc < 0) {
        int e = errno;
        if (use_pidfd) close(pfd);
        if (e == ESRCH) { puts("LMK Engine: not running"); return 0; }
        if (e == EPERM)
            fprintf(stderr, "LMK Engine: pid %d exists but no permission to signal it\n",
                    (int)m->pid);
        else
            fprintf(stderr, "signal pid %d: %s\n", (int)m->pid, strerror(e));
        return 1;
    }

    bool gone = daemon_wait_exit(use_pidfd ? pfd : -1, m, STOP_WAIT_MS);
    if (use_pidfd) close(pfd);
    if (!gone) {
        fprintf(stderr, "LMK Engine (pid %d): SIGTERM sent but still running after "
                        "%ds — not escalating; PID file kept\n",
                (int)m->pid, STOP_WAIT_MS / 1000);
        return 1;
    }
    /* The daemon removes its own file on clean shutdown; if it died
     * abnormally the leftover is stale and safe to clear (lock is free). */
    PidMeta after = pid_meta_read();
    if (pid_meta_same(&after, m)) pid_file_remove_stale(m);
    printf("LMK Engine stopped (pid %d)\n", (int)m->pid);
    return 0;
}

/* Status display */
static void print_status(void) {
    MemInfo mi;
    if (read_meminfo(&mi) != 0) { printf("Failed to read memory info\n"); return; }

    char dev[128] = {0}, sys[128] = {0};
    bool zram_found = zram_find(dev, sizeof(dev), sys, sizeof(sys));
    ZramInfo z;
    if (zram_found) {

        strncpy(g_zram_dev, dev, sizeof(g_zram_dev) - 1);
        g_zram_dev[sizeof(g_zram_dev) - 1] = '\0';
        strncpy(g_zram_sys, sys, sizeof(g_zram_sys) - 1);
        g_zram_sys[sizeof(g_zram_sys) - 1] = '\0';
        zram_read_stats(&z);
    } else {
        memset(&z, 0, sizeof(z));
    }

    DaemonProbe dprobe; daemon_probe(&dprobe, false);
    bool  daemon_running = (dprobe.state == DS_RUNNING);
    pid_t daemon_pid     = dprobe.meta.pid;

    printf("LMK Engine %s by %s\n", LMK_VERSION, LMK_AUTHOR);
    printf("Status:\n");
    printf("  Daemon     : %s\n",
           dprobe.state == DS_RUNNING ? "RUNNING" :
           dprobe.state == DS_STOPPED ? "STOPPED" : "UNKNOWN");
    if (dprobe.state == DS_UNKNOWN)
        printf("  Detail     : %s\n", dprobe.why);
    if (daemon_running) {
        printf("  PID        : %d\n", daemon_pid);
        FILE *sf = fopen(START_TIME_FILE, "r");
        if (sf) {
            long st = 0; fscanf(sf, "%ld", &st); fclose(sf);
            if (st > 0) {
                long up = (long)time(NULL) - st;
                long ud = up / 86400; up %= 86400;
                long uh = up / 3600;  up %= 3600;
                long um = up / 60;    up %= 60;
                if (ud > 0)
                    printf("  Uptime     : %ldd %ldh %ldm %lds\n", ud, uh, um, up);
                else if (uh > 0)
                    printf("  Uptime     : %ldh %ldm %lds\n", uh, um, up);
                else
                    printf("  Uptime     : %ldm %lds\n", um, up);
            }
        }
    }
    {
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            double sys_up = 0; fscanf(uf, "%lf", &sys_up); fclose(uf);
            long su = (long)sys_up;
            long sd = su / 86400; su %= 86400;
            long sh = su / 3600;  su %= 3600;
            long sm = su / 60;    su %= 60;
            if (sd > 0)
                printf("  Sys uptime : %ldd %ldh %ldm %lds\n", sd, sh, sm, su);
            else if (sh > 0)
                printf("  Sys uptime : %ldh %ldm %lds\n", sh, sm, su);
            else
                printf("  Sys uptime : %ldm %lds\n", sm, su);
        }
    }
    printf("  RAM total  : %ld MB\n", mi.total_kb / 1024);
    printf("  RAM avail  : %ld MB (%d%%)\n", mi.avail_kb / 1024, mi.avail_pct);
    printf("  RAM free   : %ld MB (%d%%)\n", mi.free_kb  / 1024, mi.free_pct);
    printf("  RAM state  : %s\n",
           mi.avail_pct < FREE_CRIT_PCT ? "CRITICAL" :
           (mi.avail_pct < FREE_LOW_PCT  ? "LOW" : "NORMAL"));

    if (zram_found && z.active) {
        printf("  ZRAM       : active on %s\n", z.dev);
        printf("  ZRAM size  : %ld MB (%d%% of RAM)\n",
               z.disksize_kb/1024, ZRAM_SIZE_PCT);
        printf("  ZRAM used  : %ld MB (%d%%)\n", z.used_kb/1024, z.used_pct);
        if (z.orig_data_kb > 0) {

            if (z.compr_data_kb > 0)
                printf("  ZRAM orig  : %ld MB (uncompressed, %.2fx ratio)\n",
                       z.orig_data_kb/1024,
                       (double)z.orig_data_kb / (double)z.compr_data_kb);
            else
                printf("  ZRAM orig  : %ld MB (uncompressed, ratio unavailable "
                       "— mm_stat field 2 unreadable)\n", z.orig_data_kb/1024);
        }
    } else if (zram_found) {
        printf("  ZRAM       : device %s exists but inactive\n", dev);
    } else {
        printf("  ZRAM       : not detected\n");
    }

    FILE *f = fopen("/proc/swaps","r");
    if (f) {
        char line[256]; bool found = false;
        fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f)) {
            char path[200], type[32]; long sz, used;
            if (sscanf(line, "%199s %31s %ld %ld", path, type, &sz, &used) == 4 &&
                strcmp(type, "file") == 0) {
                printf("  File swap  : %s\n", path);
                printf("  Swap size  : %ld MB\n", sz/1024);
                printf("  Swap used  : %ld MB (%ld%%)\n", used/1024,
                       sz > 0 ? used*100/sz : 0);
                found = true; break;
            }
        }
        if (!found) printf("  File swap  : none\n");
        fclose(f);
    }
}

static void explain_pkg(const char *pkg) {
    score_load(true);
    AppScore *s = score_lookup(pkg);

    printf("=== %s ===\n", pkg);
    if (!s) {
        printf("  No entry in %s — never scored (not yet seen foregrounded,\n"
               "  or evicted from the %d-slot table without a confirmed-\n"
               "  category protection bonus).\n", SCORE_FILE, SCORE_MAX_APPS);
    } else {
        time_t now = time(NULL);
        int score = score_compute(s);
        printf("  Score        : %d / %d\n", score, SCORE_MAX);
        printf("  Category     : %s%s\n",
               s->category == 0 ? "NATIVE" : s->category == 1 ? "SYSAPP" : "USER",
               s->category_confirmed ? " (confirmed)" : " (unconfirmed guess)");
        printf("  fg_count     : %d\n", s->fg_count);
        printf("  session_count: %d\n", s->session_count);
        printf("  restart_count: %d%s\n", s->restart_count,
               s->restart_count >= BOUNCE_SUPPRESS_RC ? "  [confirmed bouncer]" : "");
        printf("  avg_swap_kb  : %ld KB\n", s->avg_swap_kb);
        printf("  avg_session  : %ld s%s\n", s->avg_session_duration_s,
               s->avg_session_duration_s >= MIN_GENUINE_SESSION_S ? "" : " (below genuine-session floor)");
        if (s->raw_last_fg_do_not_read_directly > 0) {
            long ago = now - s->raw_last_fg_do_not_read_directly;
            printf("  last_fg      : %lds ago (%ldh %ldm)\n", ago, ago/3600, (ago%3600)/60);
        } else {
            printf("  last_fg      : never\n");
        }
        printf("  dc_kill_count: %d   adaptive_kill_count: %d\n",
               s->dc_kill_count, s->adaptive_kill_count);
    }

    bool never_kill  = name_matches(pkg, NEVER_KILL);
    bool svc_exempt  = name_matches(pkg, SERVICE_EXEMPT);
    bool launcher    = is_launcher_like(pkg);
    bool ime         = is_ime_like(pkg);
    printf("  Static class : %s\n",
           never_kill ? "NEVER_KILL" : svc_exempt ? "SERVICE_EXEMPT" :
           launcher   ? "launcher"   : ime         ? "IME" : "(none — normal candidate)");

    /* Rank cache: last snapshot the live daemon wrote (rank/swap as of its
     * last save interval, not necessarily current this instant). */
    FILE *rf = fopen(RANK_CACHE_FILE, "r");
    bool found_rank = false;
    if (rf) {
        char line[300], name[256]; long sw; int rk;
        while (fgets(line, sizeof(line), rf)) {
            if (sscanf(line, "%255s %ld %d", name, &sw, &rk) == 3 &&
                strcmp(name, pkg) == 0) {

                static const char * const RANK_NAMES[] = {
                    "EXEMPT","TIER1","TIER2","TIER3","TIER4"
                };
                printf("  Cached rank  : %s (swap=%ldKB, as of last rank-cache save)\n",
                       (rk >= 0 && rk <= 4) ? RANK_NAMES[rk] : "?", sw);
                found_rank = true;
                break;
            }
        }
        fclose(rf);
    }
    if (!found_rank) printf("  Cached rank  : not in rank cache\n");

    /* Last few kill/bounce/protect log lines mentioning this pkg. */
    FILE *lf = fopen(LOG_FILE, "r");
    if (lf) {
        char line[512];
        char hist[5][512]; int hn = 0;
        while (fgets(line, sizeof(line), lf)) {
            if (strstr(line, pkg) &&
                (strstr(line, "pid=") || strstr(line, "Resilient") ||
                 strstr(line, "Sparing") || strstr(line, "Retained"))) {
                strncpy(hist[hn % 5], line, sizeof(hist[0]) - 1);
                hist[hn % 5][sizeof(hist[0]) - 1] = 0;
                hn++;
            }
        }
        fclose(lf);
        if (hn > 0) {
            printf("  Recent log events (last %d):\n", hn < 5 ? hn : 5);
            int start = hn < 5 ? 0 : hn - 5;
            for (int i = start; i < hn; i++) {
                char *l = hist[i % 5];
                size_t len = strlen(l);
                if (len && l[len-1] == '\n') l[len-1] = 0;
                printf("    %s\n", l);
            }
        } else {
            printf("  Recent log events: none found in current log\n");
        }
    }
}

/* Usage */
static void usage(const char *prog) {
    printf(
"LMK Engine %s by %s\n"
"Usage:\n"
"  %s --start         start daemon\n"
"  %s --stop          stop daemon\n"
"  %s --status        show memory/ZRAM/swap status\n"
"  %s --force-zram-cycle [--yes-i-know]  test-trigger a ZRAM cycle\n"
"                     standalone, no live daemon needed (see notes in\n"
"                     source above --force-zram-cycle in main())\n"
"  %s --explain <pkg> show what the daemon knows about one app\n"
"  %s --log           tail live log\n"
"  %s --swap <MB>     create & enable file swap\n"
"  %s --delswap       remove file swap\n"
"\n"
"Options (opt-in, default OFF):\n"
"  PSItune  — set persist.lmk.psitune=1 or psitune=1 in\n"
"             /data/adb/lmk_engine.conf to let the daemon tune\n"
"             ro.lmk.psi_partial_stall_ms=200 and restart lmkd\n",
        LMK_VERSION, LMK_AUTHOR,
        prog, prog, prog, prog, prog, prog, prog, prog);
}

/* Main */
int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 0; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "--features")) {
        printf("LMK Engine %s by %s — feature list\n\n", LMK_VERSION, LMK_AUTHOR);
        printf("1) Score Retention — per-app usage scoring & pinning\n"
               "   - Per-app scoring (lmk_scores.dat, v9 format, dirty-gated\n"
               "     incremental writes + daily stale-entry compaction)\n"
               "   - fg_count / session_count / avg_swap_kb tracking\n"
               "   - rank blends recency with session_count frequency, so\n"
               "     frequent-but-not-recent apps don't rank as low as one-offs\n"
               "   - True foreground vs background-launch differentiation,\n"
               "     with debounced foreground-change detection (2-tick confirm)\n"
               "   - rank-based kill ordering (lmk_rank.cache, persists across restarts)\n"
               "   - OOM pinning / active retention: phased memory.low floors\n"
               "     (TIER1-only slots, then shared TIER1/TIER2 slots), tier\n"
               "     capacity enforced per-package (was a flat oom_adj step\n"
               "     threshold, then dynamic headroom-scaled oom_adj — both removed)\n"
               "   - bounce-loop suppression (restart_count + window tracking)\n"
               "   - generalized runaway-growth detection for any non-fg,\n"
               "     non-HAL, non-pinned process (was scene-daemon-shaped)\n\n");
        printf("2) Adaptive_Clean — memory & ZRAM reclaim\n"
               "   - 7-tier escalation: IDLE-DEEP / IDLE / MAINT / LOW / MED / HIGH / DEEP\n"
               "   - PSI (/proc/pressure/memory) pressure escalation\n"
               "   - PSI event-driven fast-wake (poll() on full-stall trigger,\n"
               "     same mechanism real lmkd uses — reacts to jank-causing\n"
               "     stalls within the current tick's sleep window instead of\n"
               "     waiting out the full interval)\n"
               "   - Trend detection (rising ZRAM%% over time window)\n"
               "   - T3 deep clean: kill -> drop_caches -> compact_memory -> zram/compact,\n"
               "     each stage skipped adaptively once pressure is already relieved\n"
               "   - Idle-deep intense sweep after 30 min continuous screen-off\n"
               "   - Futility tracking with auto-pause on repeated no-op deep cleans\n\n");
        printf("3) Other features\n"
               "   - PSI-driven dynamic swappiness\n"
               "   - Screen-off / Doze detection feeding idle timers\n"
               "   - Widget provider protection (multi-user XML scan + cmd fallback,\n"
               "     boot-settle grace window, parse-failure resilience)\n"
               "   - Launcher / SystemUI / IME (keyboard) hard kill guards\n"
               "   - HAL / native-daemon / bg-service-component detection\n"
               "   - Proactive launch squeeze (cgroup memory.high first, kill\n"
               "     fallback only for any shortfall), with warm-relaunch skip\n"
               "   - Adaptive main-loop polling (CPU backoff when calm)\n"
               "   - ZRAM staged bring-up with checked sysfs writes, bounded\n"
               "     cycle recovery, deferred resize; device-scoped usage stats\n"
               "   - v1.1: verified SIGKILL (pidfd), full-reset protection parity,\n"
               "     memory.high lifecycle release triggers, opt-in LMKD PSItune\n");
        return 0;
    }
    if (!strcmp(cmd, "--log")) {
        FILE *lf = fopen(LOG_FILE, "r");
        if (!lf) { fprintf(stderr,"No log at %s\n", LOG_FILE); return 1; }
        struct stat lst;
        if (fstat(fileno(lf), &lst) != 0) { perror("fstat"); fclose(lf); return 1; }
        dev_t log_dev = lst.st_dev;
        ino_t log_ino = lst.st_ino;

        /* Start 8 KiB from the end; if seeking fails or the file is
         * empty, start from the beginning. */
        long tail = 0;
        if (fseek(lf, 0, SEEK_END) == 0) {
            long fsz = ftell(lf);
            if (fsz > 8192) tail = fsz - 8192;
        }
        if (fseek(lf, tail, SEEK_SET) != 0) { rewind(lf); tail = 0; }
        char line[512];
        if (tail > 0 && !fgets(line, sizeof(line), lf)) clearerr(lf);   /* drop the partial first line */
        while (fgets(line, sizeof(line), lf)) fputs(line, stdout);
        fflush(stdout);
        printf("--- following log (Ctrl-C to stop) ---\n");
        fflush(stdout);
        for (;;) {
            while (fgets(line, sizeof(line), lf)) { fputs(line, stdout); fflush(stdout); }
            if (ferror(lf)) { fprintf(stderr, "read error on %s\n", LOG_FILE); fclose(lf); return 1; }
            clearerr(lf);   /* EOF is sticky; without this appended data is never seen */
            usleep(500000);

            /* Drain what the daemon wrote just before a rotation, then
             * follow the new file. */
            struct stat ps;
            if (stat(LOG_FILE, &ps) != 0) continue;
            if (ps.st_ino != log_ino || ps.st_dev != log_dev) {
                while (fgets(line, sizeof(line), lf)) { fputs(line, stdout); fflush(stdout); }
                FILE *nf = fopen(LOG_FILE, "r");
                if (!nf) continue;
                fclose(lf);
                lf = nf;
                log_dev = ps.st_dev;
                log_ino = ps.st_ino;
                printf("--- log rotated, following new file ---\n");
                fflush(stdout);
            } else {
                long pos = ftell(lf);
                if (pos >= 0 && ps.st_size < (off_t)pos) rewind(lf);   /* truncated in place */
            }
        }
    }
    if (!strcmp(cmd, "--swap")) {
        if (argc < 3) { fprintf(stderr,"Usage: %s --swap <SIZE_MB>\n",argv[0]); return 1; }
        int swap_mb = 0;
        if (!parse_swap_mb(argv[2], &swap_mb)) {
            fprintf(stderr, "Invalid size '%s': expected an integer from %d to %d (MB)\n",
                    argv[2], SWAP_MIN_MB, SWAP_MAX_MB);
            return 1;
        }
        return swap_create(swap_mb);
    }
    if (!strcmp(cmd, "--delswap")) { return swap_delete(); }
    if (!strcmp(cmd, "--stop"))    { return kill_daemon(); }
    if (!strcmp(cmd, "--status"))  { print_status(); return 0; }

    if (!strcmp(cmd, "--force-zram-cycle")) {
        /* Exit codes: 0 cycled + verified, 1 failure / inactive / not
         * found / bad usage, 3 refused by the avail_kb safety gate,
         * 4 refused because a daemon or another ZRAM operation is active. */
        if (argc >= 3 && strcmp(argv[2], "--yes-i-know") != 0) {
            fprintf(stderr, "Unknown option '%s' (only --yes-i-know is accepted)\n", argv[2]);
            return 1;
        }
        if (argc > 3) { fprintf(stderr, "Too many arguments\n"); return 1; }
        bool bypass_gate = (argc >= 3);

        /* One owner of ZRAM at a time: the daemon holds the singleton
         * lock for its whole life, so holding it here excludes the
         * daemon, a daemon starting up, and a second force-cycle. */
        int zlock_fd = -1, zlock_err = 0;
        LockResult zl = singleton_lock_try(&zlock_fd, &zlock_err);
        if (zl == LK_ERR) {
            fprintf(stderr, "Cannot take singleton lock %s: %s. Refusing to touch ZRAM "
                            "without serialization.\n", LOCK_FILE_PATH, strerror(zlock_err));
            return 1;
        }
        if (zl == LK_BUSY) {
            fprintf(stderr, "Refused: the LMK Engine daemon (or another ZRAM operation) "
                            "holds the singleton lock. Run --stop first, then retry.\n");
            return 4;
        }
        DaemonProbe zp; daemon_probe(&zp, true);
        if (zp.state != DS_STOPPED) {
            fprintf(stderr, "Refused: %s (%s). Run --stop first, then retry.\n",
                    zp.state == DS_RUNNING ? "a legacy daemon without lock is running"
                                           : "daemon state cannot be verified", zp.why);
            close(zlock_fd);
            return zp.state == DS_RUNNING ? 4 : 1;
        }

        MemInfo mi;
        if (read_meminfo(&mi) != 0) { fprintf(stderr, "Failed to read meminfo\n"); return 1; }
        g_total_ram_kb = mi.total_kb;
        char dev[128] = {0}, sys[128] = {0};
        if (!zram_find(dev, sizeof(dev), sys, sizeof(sys))) {
            fprintf(stderr, "No ZRAM device found (neither /dev/block/zram0 nor "
                            "/dev/zram0 exists and the zram module could not be loaded)\n");
            return 1;
        }
        strncpy(g_zram_dev, dev, sizeof(g_zram_dev) - 1);
        g_zram_dev[sizeof(g_zram_dev) - 1] = '\0';
        strncpy(g_zram_sys, sys, sizeof(g_zram_sys) - 1);
        g_zram_sys[sizeof(g_zram_sys) - 1] = '\0';

        /* "found" is not "active": ask /proc/swaps, like the daemon does. */
        g_zram_active = zram_is_active_in_proc_swaps();
        if (!g_zram_active) {
            fprintf(stderr, "ZRAM device %s exists but is INACTIVE (not listed in "
                            "/proc/swaps) — nothing to cycle\n", g_zram_dev);
            return 1;
        }
        printf(bypass_gate
               ? "Forcing ZRAM cycle on %s (bypassing avail_kb safety gate)...\n"
               : "Attempting ZRAM cycle on %s (normal gate applies — pass "
                 "--yes-i-know to bypass it for testing)...\n", g_zram_dev);
        fflush(stdout);

        ZramCycleResult cr = zram_cycle(bypass_gate);
        bool final_active = zram_is_active_in_proc_swaps();   /* final truth */
        g_zram_active = final_active;

        if (cr == ZC_OK && final_active) {
            printf("ZRAM cycle completed: %s is active in /proc/swaps\n", g_zram_dev);
            return 0;
        }
        if (cr == ZC_GATE_SKIPPED) {
            fprintf(stderr, "ZRAM cycle NOT performed: %s. ZRAM state unchanged "
                            "(%s). Re-run with --yes-i-know to bypass.\n",
                    zram_cycle_result_str(cr),
                    final_active ? "active" : "inactive");
            return 3;
        }
        fprintf(stderr, "ZRAM cycle FAILED: %s%s. Final state per /proc/swaps: %s. "
                        "See ZRAMcycle lines above/in the log.\n",
                zram_cycle_result_str(cr),
                (cr == ZC_OK && !final_active) ? " (but device missing afterwards)" : "",
                final_active ? "ACTIVE" : "INACTIVE");
        return 1;
    }
    if (!strcmp(cmd, "--explain")) {
        if (argc < 3) { fprintf(stderr,"Usage: %s --explain <package.name>\n",argv[0]); return 1; }
        explain_pkg(argv[2]); return 0;
    }

    if (!strcmp(cmd, "--start")) {
        /* take the singleton lock FIRST, then decide.
         * No check-then-fork window: of N concurrent --start calls
         * exactly one gets LK_OK. */
        int lock_fd = -1, lerr = 0;
        LockResult lr = singleton_lock_acquire(&lock_fd, &lerr, 1000);
        if (lr == LK_ERR) {
            fprintf(stderr, "LMK Engine: cannot acquire singleton lock %s: %s — "
                            "refusing to start without singleton protection\n",
                    LOCK_FILE_PATH, strerror(lerr));
            return 1;
        }
        if (lr == LK_BUSY) {
            DaemonProbe bp; daemon_probe(&bp, false);
            if (bp.state == DS_RUNNING)
                printf("LMK Engine already running (pid %d)\n", (int)bp.meta.pid);
            else
                printf("LMK Engine already running or starting "
                       "(singleton lock held by another instance)\n");
            return 0;
        }

        /* Lock owned: re-validate the PID metadata before deciding. A
         * live legacy (pre-lock) daemon is the only thing that can still
         * be running here. */
        DaemonProbe sp; daemon_probe(&sp, true);
        if (sp.state == DS_RUNNING) {
            printf("LMK Engine already running (pid %d, %s)\n",
                   (int)sp.meta.pid, sp.why);
            close(lock_fd);
            return 0;
        }
        if (sp.state == DS_UNKNOWN) {
            fprintf(stderr, "LMK Engine: cannot rule out a running instance: %s\n"
                            "Refusing to start; resolve the above and retry.\n", sp.why);
            close(lock_fd);
            return 1;
        }
        if (sp.stale_file) {
            if (unlink(PID_FILE) != 0 && errno != ENOENT) {
                fprintf(stderr, "LMK Engine: cannot remove stale %s: %s\n",
                        PID_FILE, strerror(errno));
                close(lock_fd);
                return 1;
            }
        }

        int rp[2];                       /* child -> parent readiness pipe */
        if (pipe2(rp, O_CLOEXEC) != 0) {
            perror("pipe2"); close(lock_fd); return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork"); close(rp[0]); close(rp[1]); close(lock_fd); return 1;
        }
        if (pid > 0) {
            /* Parent: drop ITS copy of the lock fd only (no LOCK_UN -
             * the child's duplicate keeps the shared lock alive). */
            close(lock_fd);
            close(rp[1]);
            char ack = 0; int got = 0;
            struct pollfd pf = { .fd = rp[0], .events = POLLIN };
            int prc;
            do { prc = poll(&pf, 1, START_CONFIRM_MS); } while (prc < 0 && errno == EINTR);
            if (prc > 0) {
                ssize_t r;
                do { r = read(rp[0], &ack, 1); } while (r < 0 && errno == EINTR);
                got = (r == 1);
            }
            close(rp[0]);
            if (got && ack == 'K') {
                printf("LMK Engine started (pid %d)\n", (int)pid);
                return 0;
            }
            fprintf(stderr, "LMK Engine: daemon (pid %d) %s — see %s\n", (int)pid,
                    prc == 0 ? "did not confirm startup in time"
                             : "failed during startup", LOG_FILE);
            return 1;
        }

        /* daemon child: owns the lock from here on */
        close(rp[0]);
        g_lock_fd = lock_fd;             /* keep open for the process lifetime */
        setsid();
        g_log = log_open_append();
        if (!g_log) g_log_reopen_after = time(NULL) + LOG_REOPEN_RETRY_S;
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        signal(SIGTERM, signal_handler);
        signal(SIGINT,  signal_handler);
        signal(SIGHUP,  signal_handler);

        char ack = 'K';
        if (!pid_file_write_mine()) {
            loge("startup: cannot publish pid/starttime to %s (%s) — aborting",
                 PID_FILE, strerror(errno));
            ack = 'F';
        }
        /* The parent may already have given up waiting: a write to a
         * closed pipe must not kill us with SIGPIPE. */
        signal(SIGPIPE, SIG_IGN);
        ssize_t wr;
        do { wr = write(rp[1], &ack, 1); } while (wr < 0 && errno == EINTR);
        int ack_err = errno;
        signal(SIGPIPE, SIG_DFL);
        close(rp[1]);
        if (ack == 'K' && wr != 1) {
            /* Parent gave up or the pipe broke: it reported failure, so
             * a daemon must not keep running unconfirmed. */
            loge("startup: ACK not delivered (%s), aborting",
                 wr < 0 ? strerror(ack_err) : "short write");
            pid_file_remove_if_mine();
        }
        if (ack != 'K' || wr != 1) {
            if (g_log) fclose(g_log);
            return 1;                     /* lock released by process exit */
        }

        run_daemon();
        pid_file_remove_if_mine();        /* only our own identity file */
        unlink(START_TIME_FILE);
        if (g_log) { FILE *lg = g_log; g_log = NULL; fclose(lg); }
        /* g_lock_fd intentionally left to the kernel: released at exit
         * (and the lock file itself is never unlinked). */
        return 0;
    }

    fprintf(stderr,"Unknown command: %s\n\n", cmd);
    usage(argv[0]); return 1;
}