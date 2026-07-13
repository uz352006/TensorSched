#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <algorithm>

#include "scheduler.h"
#include "job.h"

static constexpr int WIN_W  = 1440;
static constexpr int WIN_H  = 880;
static constexpr int FPS    = 60;
static constexpr int HDR_H  = 58;
static constexpr int SB_W   = 278;
static constexpr int PAD    = 6;

struct Color { Uint8 r, g, b, a = 255; };

static const Color C_BG      = {  8,  11,  20 };
static const Color C_PANEL   = { 14,  18,  32 };
static const Color C_PANEL2  = { 20,  26,  46 };
static const Color C_PANEL3  = { 28,  36,  60 };
static const Color C_BORDER  = { 40,  54,  92 };
static const Color C_BORDER2 = { 65,  86, 140 };

static const Color C_TEAL    = {  0, 220, 180 };
static const Color C_TEAL_D  = {  0,  90,  72 };
static const Color C_AMBER   = {255, 176,  30 };
static const Color C_AMBER_D = {120,  76,   8 };
static const Color C_CORAL   = {255,  80,  80 };
static const Color C_CORAL_D = {130,  32,  32 };
static const Color C_BLUE    = { 80, 168, 255 };
static const Color C_BLUE_D  = { 28,  72, 160 };
static const Color C_PURPLE  = {175, 145, 255 };
static const Color C_GREEN   = { 72, 230, 120 };
static const Color C_GREEN_D = { 18,  88,  44 };
static const Color C_PINK    = {255, 110, 200 };
static const Color C_GOLD    = {255, 210,  50 };

static const Color C_TEXT    = {220, 235, 255 };
static const Color C_TEXT2   = {125, 155, 200 };
static const Color C_TEXT3   = { 58,  80, 126 };
static const Color C_TEXT4   = { 36,  50,  86 };

static const Color MLFQ_COL[3] = { C_TEAL, C_AMBER, C_CORAL };
static const Color PRIO_COL[4] = { C_TEXT2, C_BLUE, C_AMBER, C_CORAL };

static const Color LOG_COLS[6] = {
    C_TEXT2,
    C_TEAL,
    C_GREEN,
    C_CORAL,
    C_PINK,
    C_PURPLE,
};

static Color statusColor(JobStatus s) {
    switch (s) {
        case JobStatus::WAITING:   return C_TEXT3;
        case JobStatus::READY:     return C_AMBER;
        case JobStatus::RUNNING:   return C_TEAL;
        case JobStatus::PREEMPTED: return C_PINK;
        case JobStatus::COMPLETED: return C_GREEN;
        case JobStatus::FAILED:    return C_CORAL;
    }
    return C_TEXT2;
}

static Color priorityColor(JobPriority p) {
    int idx = static_cast<int>(p) - 1;
    return PRIO_COL[(idx >= 0 && idx < 4) ? idx : 0];
}

static void setColor(SDL_Renderer* r, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void fillRect(SDL_Renderer* r, int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    setColor(r, c);
    SDL_Rect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

static void drawRect(SDL_Renderer* r, int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    setColor(r, c);
    SDL_Rect rect{x, y, w, h};
    SDL_RenderDrawRect(r, &rect);
}

static void fillRoundRect(SDL_Renderer* r, int x, int y, int w, int h,
                           int radius, Color c) {
    if (w <= 0 || h <= 0) return;
    radius = std::min(radius, std::min(w / 2, h / 2));
    if (radius < 1) { fillRect(r, x, y, w, h, c); return; }
    setColor(r, c);
    SDL_Rect body{x + radius, y, w - 2 * radius, h};
    SDL_RenderFillRect(r, &body);
    SDL_Rect left{x, y + radius, radius, h - 2 * radius};
    SDL_RenderFillRect(r, &left);
    SDL_Rect right{x + w - radius, y + radius, radius, h - 2 * radius};
    SDL_RenderFillRect(r, &right);
    for (int dy = 0; dy < radius; dy++) {
        double ang = std::acos(1.0 - (double)dy / radius);
        int    dx  = (int)(radius * std::sin(ang));
        SDL_RenderDrawLine(r, x + radius - dx, y + dy,
                              x + w - radius + dx - 1, y + dy);
        SDL_RenderDrawLine(r, x + radius - dx, y + h - 1 - dy,
                              x + w - radius + dx - 1, y + h - 1 - dy);
    }
}

static void drawProgressBar(SDL_Renderer* r, int x, int y, int w, int h,
                             double pct, Color bg, Color fg, Color border,
                             bool glow = false) {
    if (w <= 0 || h <= 0) return;
    fillRoundRect(r, x, y, w, h, h / 2, bg);
    int filled = (int)(w * std::min(std::max(pct, 0.0), 100.0) / 100.0);
    if (filled > 0) fillRoundRect(r, x, y, filled, h, h / 2, fg);
    drawRect(r, x, y, w, h, border);
    if (glow && filled > 0) {
        Color gl = {fg.r, fg.g, fg.b, 35};
        fillRoundRect(r, x - 1, y - 1, filled + 2, h + 2, h / 2, gl);
    }
}

struct Fonts {
    TTF_Font* large  = nullptr;
    TTF_Font* medium = nullptr;
    TTF_Font* small  = nullptr;
    TTF_Font* tiny   = nullptr;
    TTF_Font* mono   = nullptr;
    TTF_Font* title  = nullptr;

    bool load(const char* sans, const char* mono_path) {
        large  = TTF_OpenFont(sans, 20);
        medium = TTF_OpenFont(sans, 14);
        small  = TTF_OpenFont(sans, 12);
        tiny   = TTF_OpenFont(sans, 10);
        title  = TTF_OpenFont(sans, 28);
        mono   = TTF_OpenFont(mono_path, 11);
        return large && medium && small && title && mono;
    }

    bool loadSystem() {
        const char* sans_paths[] = {
            "fonts/Rajdhani-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        };
        const char* mono_paths[] = {
            "fonts/JetBrainsMono-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
            "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        };
        close();
        for (const char* s : sans_paths) {
            for (const char* m : mono_paths) {
                if (load(s, m)) return true;
                close();
            }
        }
        return false;
    }

    void close() {
        auto fc = [](TTF_Font*& f){ if(f){ TTF_CloseFont(f); f=nullptr; } };
        fc(large); fc(medium); fc(small); fc(tiny); fc(title); fc(mono);
    }

    TTF_Font* safe(TTF_Font* f) const {
        if (f) return f;
        if (small) return small;
        if (medium) return medium;
        return tiny;
    }
};

static void renderText(SDL_Renderer* r, TTF_Font* f,
                        const char* t, int x, int y, Color c,
                        bool right_align = false) {
    if (!f || !t || !t[0]) return;
    SDL_Color sc{c.r, c.g, c.b, c.a};
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, t, sc);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    int tw = surf->w, th = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_SetTextureAlphaMod(tex, c.a);
    SDL_Rect dst{right_align ? x - tw : x, y, tw, th};
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

static void renderTextStr(SDL_Renderer* r, TTF_Font* f,
                           const std::string& t, int x, int y, Color c,
                           bool right_align = false) {
    renderText(r, f, t.c_str(), x, y, c, right_align);
}

static int textW(TTF_Font* f, const char* t) {
    if (!f || !t) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, t, &w, &h);
    return w;
}

static int textH(TTF_Font* f) {
    if (!f) return 12;
    return TTF_FontHeight(f);
}

static constexpr int SPARK_LEN = 120;

struct SparkData {
    float data[SPARK_LEN] = {};
    int   head = 0;

    void push(float v) {
        data[head] = v;
        head = (head + 1) % SPARK_LEN;
    }

    float get(int i) const { return data[(head + i) % SPARK_LEN]; }
};

static void drawSparkline(SDL_Renderer* r, int x, int y, int w, int h,
                           const SparkData& s, Color line, Color fill,
                           float max_val = 100.0f) {
    if (w <= 2 || h <= 2 || max_val <= 0.0f) return;
    int pts = SPARK_LEN;
    setColor(r, fill);
    for (int i = 0; i < pts - 1; i++) {
        float x0 = x + (float)i       / (pts - 1) * w;
        float x1 = x + (float)(i + 1) / (pts - 1) * w;
        float v0 = s.get(i)     / max_val;
        float v1 = s.get(i + 1) / max_val;
        if (v0 > 1.0f) v0 = 1.0f;
        if (v1 > 1.0f) v1 = 1.0f;
        float y0f = y + h - v0 * h;
        float y1f = y + h - v1 * h;
        int   px0 = (int)x0, px1 = (int)x1;
        for (int px = px0; px <= px1; px++) {
            float t  = (px1 == px0) ? 0.0f : (float)(px - px0) / (px1 - px0);
            int   vy = (int)(y0f + t * (y1f - y0f));
            SDL_RenderDrawLine(r, px, vy, px, y + h);
        }
    }
    setColor(r, line);
    for (int i = 0; i < pts - 1; i++) {
        int ix0 = x + (int)((float)i       / (pts - 1) * w);
        int ix1 = x + (int)((float)(i + 1) / (pts - 1) * w);
        float v0 = s.get(i)     / max_val; if (v0 > 1.0f) v0 = 1.0f;
        float v1 = s.get(i + 1) / max_val; if (v1 > 1.0f) v1 = 1.0f;
        int   iy0 = y + h - (int)(v0 * h);
        int   iy1 = y + h - (int)(v1 * h);
        SDL_RenderDrawLine(r, ix0, iy0, ix1, iy1);
    }
}

struct FormState {
    char name_buf[60] = "Job";
    int  prio_idx   = 2;
    int  cpu        = 20;
    int  memory     = 256;
    int  burst      = 10;
    int  focused    = -1;
    bool submit_flash = false;
    Uint32 flash_ms   = 0;

    static const char* prios[4];
};
const char* FormState::prios[4] = {"LOW", "MEDIUM", "HIGH", "CRITICAL"};

struct UILogEntry {
    char   text[256];
    int    color_hint;
    Uint32 born_ms;
};

static constexpr int UI_LOG_MAX = 512;

struct UIState {
    int   log_scroll   = 0;
    int   job_scroll   = 0;
    int   gantt_scroll = 0;
    int   active_tab   = 0;
    Uint32 start_ms    = 0;
    Uint32 last_spark_ms = 0;

    SparkData cpu_spark, mem_spark;
    SparkData mlfq_spark[3];
    SparkData preempt_spark;

    UILogEntry  log_entries[UI_LOG_MAX];
    int         log_head  = 0;
    int         log_count = 0;
    pthread_mutex_t log_mutex;

    UIState()  { pthread_mutex_init(&log_mutex, nullptr); }
    ~UIState() { pthread_mutex_destroy(&log_mutex); }

    void pushLog(const char* msg, int color_hint) {
        pthread_mutex_lock(&log_mutex);
        UILogEntry& e = log_entries[log_head % UI_LOG_MAX];
        std::strncpy(e.text, msg, sizeof(e.text) - 1);
        e.text[sizeof(e.text) - 1] = '\0';
        e.color_hint = color_hint;
        e.born_ms    = SDL_GetTicks();
        log_head++;
        if (log_count < UI_LOG_MAX) log_count++;
        log_scroll = 0;
        pthread_mutex_unlock(&log_mutex);
    }

    int logSnapshot(UILogEntry* buf, int max_count) const {
        pthread_mutex_lock(const_cast<pthread_mutex_t*>(&log_mutex));
        int count = (log_count < max_count) ? log_count : max_count;
        int start = (log_head - count + UI_LOG_MAX) % UI_LOG_MAX;
        for (int i = 0; i < count; i++)
            buf[i] = log_entries[(start + i) % UI_LOG_MAX];
        pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&log_mutex));
        return count;
    }
};

static int g_next_job_id = 1;

static std::string fitText(TTF_Font* f, const char* t, int max_px) {
    std::string s(t);
    while (!s.empty() && textW(f, s.c_str()) > max_px)
        s.pop_back();
    return s;
}

static void drawHeader(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                        UIState& ui) {
    fillRect(r, 0, 0, WIN_W, HDR_H, C_PANEL);
    fillRect(r, 0, HDR_H - 2, WIN_W, 2, C_TEAL_D);
    fillRect(r, 0, HDR_H,     WIN_W, 1, C_BORDER);

    renderText(r, f.title, "TENSOR", 16, 10, C_TEAL);
    int tx = 16 + textW(f.title, "TENSOR") + 6;
    renderText(r, f.title, "SCHED", tx, 10, C_TEXT);
    int vx = tx + textW(f.title, "SCHED") + 10;
    renderText(r, f.small, "v3.0", vx, 18, C_TEXT3);

    renderText(r, f.tiny,
        "MLFQ  ·  PRIORITY QUEUE  ·  ROUND-ROBIN  ·  PREEMPTION  ·  AGING  ·  PTHREADS  ·  MUTEX  ·  SEMAPHORE",
        16, 42, C_TEXT3);

    auto& res   = sched.resources();
    auto& stats = sched.stats();

    int rx = WIN_W - 12;

    Uint32 up = (SDL_GetTicks() - ui.start_ms) / 1000;
    char ubuf[32];
    std::snprintf(ubuf, sizeof(ubuf), "UP %02d:%02d:%02d", up/3600, (up%3600)/60, up%60);
    renderText(r, f.small, ubuf, rx, 40, C_TEXT3, true);

    auto rstat = [&](const char* lbl, const char* val, Color vc) {
        rx -= textW(f.medium, val) + 5;
        renderText(r, f.medium, val, rx, 8, vc);
        rx -= textW(f.small, lbl) + 4;
        renderText(r, f.small, lbl, rx, 11, C_TEXT3);
        rx -= 18;
        fillRect(r, rx + 12, 6, 1, 36, C_BORDER);
        rx -= 2;
    };

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d/%d", res.activeJobs(), MAX_CONCURRENT);
    rstat("ACTIVE",  buf, C_TEAL);
    rstat("DONE",    std::to_string(stats.total_completed.load()).c_str(), C_GREEN);
    rstat("FAILED",  std::to_string(stats.total_failed.load()).c_str(),   C_CORAL);
    rstat("PREEMPT", std::to_string(stats.total_preemptions.load()).c_str(), C_PINK);
    rstat("AGING",   std::to_string(stats.total_aging_promotions.load()).c_str(), C_PURPLE);
    rstat("QUEUE",   std::to_string(sched.queue().size()).c_str(), C_AMBER);
}

static void drawResourcePanel(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                               UIState& ui, int x, int y, int w, int h) {
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "SYSTEM RESOURCES", x + 10, y + 8, C_TEXT3);

    auto& res = sched.resources();
    int gy    = y + 26;

    renderText(r, f.medium, "CPU", x + 10, gy, C_TEXT2);
    char cpuv[16];
    std::snprintf(cpuv, sizeof(cpuv), "%d%%", res.usedCPU());
    renderText(r, f.medium, cpuv, x + w - 10, gy, C_TEAL, true);
    drawProgressBar(r, x + 10, gy + 18, w - 20, 8,
                    res.cpuPct(), C_PANEL2, C_TEAL, C_BORDER, true);
    Color cf1 = {0, 180, 150, 28};
    drawSparkline(r, x + 10, gy + 30, w - 20, 24, ui.cpu_spark, C_TEAL, cf1);

    gy += 60;
    renderText(r, f.medium, "MEMORY", x + 10, gy, C_TEXT2);
    char memv[24];
    std::snprintf(memv, sizeof(memv), "%dMB", res.usedMemory());
    renderText(r, f.medium, memv, x + w - 10, gy, C_AMBER, true);
    drawProgressBar(r, x + 10, gy + 18, w - 20, 8,
                    res.memoryPct(), C_PANEL2, C_AMBER, C_BORDER, true);
    Color cf2 = {255, 176, 30, 22};
    drawSparkline(r, x + 10, gy + 30, w - 20, 24, ui.mem_spark, C_AMBER, cf2);

    gy += 62;
    fillRect(r, x + 8, gy, w - 16, 1, C_BORDER);
    gy += 8;

    auto& s = sched.stats();
    auto dstat = [&](const char* lbl, const char* val, Color vc, int sy) {
        renderText(r, f.tiny, lbl, x + 10, sy, C_TEXT3);
        renderText(r, f.tiny, val, x + w - 10, sy, vc, true);
    };

    char buf[32];
    dstat("Submitted",    std::to_string(s.total_submitted.load()).c_str(), C_TEXT2,  gy);
    dstat("Completed",    std::to_string(s.total_completed.load()).c_str(), C_GREEN,  gy + 14);
    dstat("Failed",       std::to_string(s.total_failed.load()).c_str(),    C_CORAL,  gy + 28);
    dstat("Preemptions",  std::to_string(s.total_preemptions.load()).c_str(), C_PINK, gy + 42);
    dstat("Aging Promos", std::to_string(s.total_aging_promotions.load()).c_str(), C_PURPLE, gy + 56);

    std::snprintf(buf, sizeof(buf), "%.1ft", s.avg_wait_ticks.load());
    dstat("Avg Wait",       buf, C_AMBER, gy + 72);
    std::snprintf(buf, sizeof(buf), "%.1fs", s.avg_turnaround_sec.load());
    dstat("Avg Turnaround", buf, C_BLUE,  gy + 86);
    std::snprintf(buf, sizeof(buf), "%.2f j/s", s.throughput.load());
    dstat("Throughput",     buf, C_TEAL,  gy + 100);
    std::snprintf(buf, sizeof(buf), "%.1f%%", s.cpu_utilization.load());
    dstat("CPU Util",       buf, C_GREEN, gy + 114);
}

static void drawMLFQPanel(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                           UIState& ui, int x, int y, int w, int h) {
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "MLFQ LEVELS", x + 10, y + 8, C_TEXT3);

    const char* labels[3] = {
        "L0  Q=2t (Interactive)",
        "L1  Q=4t (Normal)",
        "L2  Q=8t (Background)"
    };

    for (int l = 0; l < MLFQ_LEVELS; l++) {
        int  gy  = y + 28 + l * 58;
        int  cnt = sched.queue().sizeAtLevel(l);
        Color lc = MLFQ_COL[l];

        renderText(r, f.small, labels[l], x + 10, gy, lc);
        char cntbuf[16];
        std::snprintf(cntbuf, sizeof(cntbuf), "%d job%s", cnt, cnt == 1 ? "" : "s");
        renderText(r, f.small, cntbuf, x + w - 10, gy, lc, true);

        drawProgressBar(r, x + 10, gy + 16, w - 20, 8,
                        (cnt * 10.0 > 100.0 ? 100.0 : cnt * 10.0),
                        C_PANEL2, lc, {lc.r, lc.g, lc.b, 80}, true);

        Color fl = {lc.r, lc.g, lc.b, 28};
        Color ln = {lc.r, lc.g, lc.b, 150};
        drawSparkline(r, x + 10, gy + 28, w - 20, 20, ui.mlfq_spark[l], ln, fl, 8.0f);
    }
}

static void drawConceptsPanel(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                               int x, int y, int w, int h) {
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "OS CONCEPTS ACTIVE", x + 10, y + 8, C_TEXT3);

    struct Row { const char* icon; const char* name; const char* desc; Color col; };

    auto& s = sched.stats();
    char sbuf[32], pbuf[32];
    std::snprintf(sbuf, sizeof(sbuf), "%d/%d free slots",
                  MAX_CONCURRENT - sched.resources().activeJobs(), MAX_CONCURRENT);
    std::snprintf(pbuf, sizeof(pbuf), "%d total", s.total_preemptions.load());

    const Row rows[] = {
        {"T", "pthreads",   "Worker + dispatch + aging", C_TEAL  },
        {"M", "Mutex",      "4 locks (queue/jobs/log…)", C_BLUE  },
        {"S", "Semaphore",  sbuf,                         C_PURPLE},
        {"P", "Preemption", pbuf,                         C_PINK  },
        {"A", "Aging",      "Anti-starvation promotion",  C_AMBER },
        {"Q", "Priority-Q", "CRIT>HIGH>MED>LOW per lvl",  C_CORAL },
        {"L", "MLFQ",       "3-level feedback queue",     C_GREEN },
        {"R", "Round-Robin","FIFO among equal priority",  C_GOLD  },
    };
    static constexpr int NR = 8;

    int gy = y + 26;
    for (int i = 0; i < NR; i++) {
        const Row& row = rows[i];
        Color bc = {row.col.r, row.col.g, row.col.b, 40};
        Color bd = {row.col.r, row.col.g, row.col.b, 100};
        fillRoundRect(r, x + 10, gy, 18, 16, 3, bc);
        drawRect(r, x + 10, gy, 18, 16, bd);
        renderText(r, f.tiny, row.icon, x + 14, gy + 3, row.col);
        renderText(r, f.small, row.name, x + 32, gy, C_TEXT2);
        renderText(r, f.tiny,  row.desc, x + 32, gy + 14, {row.col.r, row.col.g, row.col.b, 160});
        gy += 28;
    }
}

static bool drawFormPanel(SDL_Renderer* r, Fonts& f,
                           FormState& form, Scheduler& sched,
                           int x, int y, int w, int h,
                           const SDL_Event* ev) {
    bool submitted = false;
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "SUBMIT JOB", x + 10, y + 8, C_TEXT3);

    int fy = y + 26;

    renderText(r, f.tiny, "Job Name", x + 10, fy, C_TEXT3);
    bool nfoc = (form.focused == 0);
    fillRoundRect(r, x + 10, fy + 12, w - 20, 22, 4, C_PANEL2);
    drawRect(r, x + 10, fy + 12, w - 20, 22, nfoc ? C_TEAL : C_BORDER);
    renderText(r, f.small, form.name_buf, x + 16, fy + 16, C_TEXT);
    if (nfoc && (SDL_GetTicks() / 500) % 2 == 0) {
        int cw_pos = 16 + textW(f.small, form.name_buf);
        fillRect(r, x + cw_pos, fy + 17, 2, 12, C_TEAL);
    }
    if (ev && ev->type == SDL_MOUSEBUTTONDOWN) {
        int mx = ev->button.x, my = ev->button.y;
        if (mx >= x+10 && mx < x+w-10 && my >= fy+12 && my < fy+34)
            form.focused = 0;
        else if (form.focused == 0)
            form.focused = -1;
    }
    fy += 42;

    renderText(r, f.tiny, "Priority", x + 10, fy, C_TEXT3);
    int bw = (w - 20 - 3 * 4) / 4;
    for (int i = 0; i < 4; i++) {
        int bx  = x + 10 + i * (bw + 4);
        bool sel = (form.prio_idx == i);
        Color pc = priorityColor(static_cast<JobPriority>(i + 1));
        Color bg = sel ? Color{pc.r, pc.g, pc.b, 55} : C_PANEL2;
        fillRoundRect(r, bx, fy + 12, bw, 22, 4, bg);
        drawRect(r, bx, fy + 12, bw, 22, sel ? pc : C_BORDER);
        int lw = textW(f.tiny, FormState::prios[i]);
        renderText(r, f.tiny, FormState::prios[i],
                   bx + (bw - lw) / 2, fy + 17,
                   sel ? pc : C_TEXT3);
        if (ev && ev->type == SDL_MOUSEBUTTONDOWN) {
            int mx = ev->button.x, my = ev->button.y;
            if (mx >= bx && mx < bx+bw && my >= fy+12 && my < fy+34)
                form.prio_idx = i;
        }
    }
    fy += 42;

    auto slider = [&](const char* lbl, int& val, int minv, int maxv,
                      int fid, const char* unit, Color col) {
        renderText(r, f.tiny, lbl, x + 10, fy, C_TEXT3);
        char vbuf[16];
        std::snprintf(vbuf, sizeof(vbuf), "%d%s", val, unit);
        renderText(r, f.tiny, vbuf, x + w - 10, fy, col, true);

        int sx = x + 10, sw = w - 20, sy = fy + 12;
        fillRoundRect(r, sx, sy, sw, 6, 3, C_PANEL2);
        int filled = (int)((float)(val - minv) / (maxv - minv) * sw);
        if (filled > 0) fillRoundRect(r, sx, sy, filled, 6, 3, col);
        drawRect(r, sx, sy, sw, 6, C_BORDER);
        int thumb_x = sx + filled - 5;
        Color tc = (form.focused == fid) ? col : C_TEXT3;
        fillRoundRect(r, thumb_x, sy - 3, 10, 12, 5, tc);

        if (ev) {
            if (ev->type == SDL_MOUSEBUTTONDOWN) {
                int mx = ev->button.x, my = ev->button.y;
                if (mx >= sx && mx < sx+sw && my >= sy-4 && my < sy+10) {
                    form.focused = fid;
                    float t = (float)(mx - sx) / sw;
                    val = minv + (int)(t * (maxv - minv) + 0.5f);
                    val = std::max(minv, std::min(maxv, val));
                }
            }
            if (ev->type == SDL_MOUSEMOTION &&
                (ev->motion.state & SDL_BUTTON_LMASK) &&
                form.focused == fid) {
                float t = (float)(ev->motion.x - sx) / sw;
                val = minv + (int)(t * (maxv - minv) + 0.5f);
                val = std::max(minv, std::min(maxv, val));
            }
        }
        fy += 32;
    };

    slider("CPU %",       form.cpu,    5,   80, 2, "%",  C_TEAL);
    slider("Memory MB",   form.memory, 64, 1024, 3, "MB", C_AMBER);
    slider("Burst Ticks", form.burst,  2,   40, 4, "t",  C_PURPLE);

    int btnY = y + h - 46;
    if (btnY < fy + 6) btnY = fy + 6;

    bool hov = false;
    if (ev && ev->type == SDL_MOUSEMOTION) {
        int mx = ev->motion.x, my = ev->motion.y;
        hov = (mx >= x+10 && mx < x+w-10 && my >= btnY && my < btnY+34);
    }

    Color btnBg = C_PANEL3;
    Color btnBd = C_TEAL;
    if (form.submit_flash && SDL_GetTicks() - form.flash_ms < 300) {
        btnBg = C_GREEN_D;
        btnBd = C_GREEN;
    } else {
        form.submit_flash = false;
        if (hov) btnBg = C_TEAL_D;
    }

    fillRoundRect(r, x + 10, btnY, w - 20, 34, 6, btnBg);
    drawRect(r, x + 10, btnY, w - 20, 34, btnBd);
    const char* bl = "► SUBMIT JOB";
    int blw = textW(f.small, bl);
    renderText(r, f.small, bl, x + 10 + (w - 20 - blw) / 2, btnY + 9, btnBd);

    if (ev && ev->type == SDL_MOUSEBUTTONDOWN) {
        int mx = ev->button.x, my = ev->button.y;
        if (mx >= x+10 && mx < x+w-10 && my >= btnY && my < btnY+34) {
            char jname[80];
            std::snprintf(jname, sizeof(jname), "%s-%d",
                          form.name_buf[0] ? form.name_buf : "Job",
                          g_next_job_id);
            Job* job = new Job(
                g_next_job_id++, jname,
                static_cast<JobPriority>(form.prio_idx + 1),
                form.cpu, form.memory, form.burst);
            sched.submitJob(job);
            submitted = true;
            form.submit_flash = true;
            form.flash_ms     = SDL_GetTicks();
        }
    }
    return submitted;
}

struct Preset {
    const char* lbl;
    const char* name;
    JobPriority prio;
    int cpu, mem, burst;
    Color col;
};

static const Preset PRESETS[] = {
    {"MATRIX MUL", "MatMul",   JobPriority::HIGH,     30,  512, 20, {  0, 220, 180 } },
    {"SORT",       "Sort",     JobPriority::MEDIUM,   15,  128, 10, { 80, 168, 255 } },
    {"ML TRAIN",   "MLTrain",  JobPriority::CRITICAL, 60,  768, 35, {255,  80,  80 } },
    {"FILE IDX",   "FileIdx",  JobPriority::LOW,      10,   64,  8, {125, 155, 200 } },
    {"COMPRESS",   "Compress", JobPriority::MEDIUM,   25,  256, 14, {175, 145, 255 } },
    {"RENDER",     "Render",   JobPriority::HIGH,     45,  512, 28, {255, 176,  30 } },
    {"CHECKPOINT", "CkPt",     JobPriority::CRITICAL, 70, 1024, 40, {255, 110, 200 } },
    {"DAEMON",     "Daemon",   JobPriority::LOW,       8,   64,  6, { 72, 230, 120 } },
};
static constexpr int N_PRESETS = 8;

static void drawPresets(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                         int x, int y, int w, const SDL_Event* ev) {
    renderText(r, f.tiny, "QUICK SUBMIT PRESETS", x, y - 14, C_TEXT3);
    int bw = (w - (N_PRESETS - 1) * 5) / N_PRESETS;

    for (int i = 0; i < N_PRESETS; i++) {
        const Preset& p = PRESETS[i];
        int bx = x + i * (bw + 5);

        bool hov = false;
        if (ev && ev->type == SDL_MOUSEMOTION) {
            hov = (ev->motion.x >= bx && ev->motion.x < bx + bw &&
                   ev->motion.y >= y  && ev->motion.y < y + 40);
        }

        Color bg = hov ? Color{p.col.r, p.col.g, p.col.b, 55}
                       : Color{p.col.r, p.col.g, p.col.b, 18};
        fillRoundRect(r, bx, y, bw, 40, 5, bg);
        drawRect(r, bx, y, bw, 40, {p.col.r, p.col.g, p.col.b, 110});

        int lw = textW(f.tiny, p.lbl);
        renderText(r, f.tiny, p.lbl, bx + (bw - lw) / 2, y + 5, p.col);

        char sub[20];
        std::snprintf(sub, sizeof(sub), "%d%% %dMB", p.cpu, p.mem);
        int sw2 = textW(f.tiny, sub);
        renderText(r, f.tiny, sub, bx + (bw - sw2) / 2, y + 22, C_TEXT3);

        if (ev && ev->type == SDL_MOUSEBUTTONDOWN) {
            int mx = ev->button.x, my = ev->button.y;
            if (mx >= bx && mx < bx+bw && my >= y && my < y+40) {
                char jname[80];
                std::snprintf(jname, sizeof(jname), "%s-%d", p.name, g_next_job_id);
                Job* job = new Job(g_next_job_id++, jname, p.prio,
                                   p.cpu, p.mem, p.burst);
                sched.submitJob(job);
            }
        }
    }
}

static void drawWorkerSlots(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                             int x, int y, int w) {
    renderText(r, f.small, "WORKER THREADS", x, y, C_TEXT3);
    int slot_w = (w - (MAX_CONCURRENT - 1) * 6) / MAX_CONCURRENT;

    for (int i = 0; i < MAX_CONCURRENT; i++) {
        int sx = x + i * (slot_w + 6);
        const WorkerSlot& ws = sched.resources().getWorker(i);
        bool busy = ws.busy.load();

        Color bc = busy ? MLFQ_COL[ws.mlfq_level.load()] : C_BORDER;
        fillRoundRect(r, sx, y + 18, slot_w, 54, 6, C_PANEL2);
        drawRect(r, sx, y + 18, slot_w, 54, bc);

        if (busy)
            fillRect(r, sx, y + 18, 3, 54, MLFQ_COL[ws.mlfq_level.load()]);

        char wlbl[8];
        std::snprintf(wlbl, sizeof(wlbl), "W%d", i);
        renderText(r, f.tiny, wlbl, sx + 7, y + 22, busy ? C_TEXT : C_TEXT3);

        if (busy) {
            std::string jn = fitText(f.tiny, ws.job_name, slot_w - 14);
            renderText(r, f.tiny, jn.c_str(), sx + 7, y + 36, C_TEXT);

            char lvlb[4];
            std::snprintf(lvlb, sizeof(lvlb), "L%d", ws.mlfq_level.load());
            renderText(r, f.tiny, lvlb, sx + 7, y + 51, MLFQ_COL[ws.mlfq_level.load()]);

            int pidx = ws.priority.load() - 1;
            Color pc  = PRIO_COL[(pidx >= 0 && pidx < 4) ? pidx : 0];
            fillRoundRect(r, sx + slot_w - 14, y + 22, 9, 9, 4, pc);
        } else {
            int fw = textW(f.tiny, "FREE");
            renderText(r, f.tiny, "FREE", sx + (slot_w - fw) / 2, y + 37, C_TEXT4);
        }
    }
}

static void drawJobTable(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                          UIState& ui, int x, int y, int w, int h) {
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "JOB TABLE", x + 10, y + 8, C_TEXT3);

    int cid   = 38;
    int cname = 148;
    int cprio = 70;
    int cstat = 82;
    int clvl  = 46;
    int ccpu  = 50;
    int cmem  = 60;
    int cprog = w - cid - cname - cprio - cstat - clvl - ccpu - cmem - 28;
    if (cprog < 60) cprog = 60;

    int hx = x + 8, hy = y + 26;
    fillRect(r, x + 4, hy, w - 8, 20, C_PANEL2);

    auto colH = [&](const char* t, int& cx, int cw) {
        renderText(r, f.tiny, t, cx + 4, hy + 5, C_TEXT3);
        cx += cw;
    };
    colH("ID",       hx, cid);
    colH("Name",     hx, cname);
    colH("PRIORITY", hx, cprio);
    colH("STATUS",   hx, cstat);
    colH("MLFQ",     hx, clvl);
    colH("CPU",      hx, ccpu);
    colH("MEM",      hx, cmem);
    colH("PROGRESS", hx, cprog);

    int clip_y = hy + 22;
    int clip_h = h - (clip_y - y) - 4;
    SDL_Rect clip{x, clip_y, w, clip_h};
    SDL_RenderSetClipRect(r, &clip);

    static Job* jobs_buf[MAX_JOBS];
    int job_count = sched.allJobsSnapshot(jobs_buf, MAX_JOBS);

    auto rankStatus = [](JobStatus s) -> int {
        switch (s) {
            case JobStatus::RUNNING:   return 0;
            case JobStatus::PREEMPTED: return 1;
            case JobStatus::READY:     return 2;
            case JobStatus::WAITING:   return 3;
            case JobStatus::COMPLETED: return 4;
            case JobStatus::FAILED:    return 5;
        }
        return 6;
    };

    for (int i = 1; i < job_count; i++) {
        Job* key = jobs_buf[i];
        int  j   = i - 1;
        while (j >= 0 && rankStatus(jobs_buf[j]->status.load()) >
                         rankStatus(key->status.load())) {
            jobs_buf[j + 1] = jobs_buf[j];
            j--;
        }
        jobs_buf[j + 1] = key;
    }

    int row_h   = 34;
    int total_h = job_count * row_h;
    int max_scr = std::max(0, total_h - clip_h);
    if (ui.job_scroll > max_scr) ui.job_scroll = max_scr;

    int ry = clip_y - ui.job_scroll;

    for (int ji = 0; ji < job_count; ji++) {
        Job* job = jobs_buf[ji];
        if (ry + row_h < clip_y || ry > clip_y + clip_h) { ry += row_h; continue; }

        JobStatus jstat = job->status.load();
        Color rowBg = (jstat == JobStatus::RUNNING) ? C_PANEL3 : C_PANEL;
        fillRect(r, x + 4, ry, w - 8, row_h - 2, rowBg);

        if (jstat == JobStatus::RUNNING)
            fillRect(r, x + 4, ry, 3, row_h - 2, MLFQ_COL[job->mlfq_level.load()]);
        else if (jstat == JobStatus::PREEMPTED)
            fillRect(r, x + 4, ry, 3, row_h - 2, C_PINK);

        int cx = x + 8;
        char idbuf[8]; std::snprintf(idbuf, sizeof(idbuf), "#%d", job->id);
        renderText(r, f.tiny, idbuf, cx + 2, ry + 10, C_TEXT3);
        cx += cid;

        std::string nm = fitText(f.small, job->name, cname - 8);
        renderText(r, f.small, nm.c_str(), cx + 2, ry + 8, C_TEXT);
        cx += cname;

        Color pc = priorityColor(job->priority);
        fillRoundRect(r, cx + 2, ry + 7, cprio - 8, 18, 3, {pc.r, pc.g, pc.b, 25});
        drawRect(r, cx + 2, ry + 7, cprio - 8, 18, {pc.r, pc.g, pc.b, 90});
        renderText(r, f.tiny, Job::priorityStr(job->priority), cx + 4, ry + 11, pc);
        cx += cprio;

        Color sc2 = statusColor(jstat);
        renderText(r, f.tiny, Job::statusStr(jstat), cx + 4, ry + 11, sc2);
        cx += cstat;

        int lvl = job->mlfq_level.load();
        if (lvl < 0) lvl = 0; if (lvl >= MLFQ_LEVELS) lvl = MLFQ_LEVELS - 1;
        Color lc = MLFQ_COL[lvl];
        char lvlb[4]; std::snprintf(lvlb, sizeof(lvlb), "L%d", lvl);
        fillRoundRect(r, cx + 2, ry + 7, clvl - 8, 18, 3, {lc.r, lc.g, lc.b, 28});
        renderText(r, f.tiny, lvlb, cx + 5, ry + 11, lc);
        cx += clvl;

        char cpubuf[8]; std::snprintf(cpubuf, sizeof(cpubuf), "%d%%", job->cpu_required);
        renderText(r, f.tiny, cpubuf, cx + 2, ry + 11, C_TEAL);
        cx += ccpu;

        char membuf[10]; std::snprintf(membuf, sizeof(membuf), "%dMB", job->memory_required);
        renderText(r, f.tiny, membuf, cx + 2, ry + 11, C_AMBER);
        cx += cmem;

        double pct = job->completionPct();
        Color pfg = (jstat == JobStatus::COMPLETED) ? C_GREEN :
                    (jstat == JobStatus::FAILED)    ? C_CORAL :
                    (jstat == JobStatus::PREEMPTED) ? C_PINK  : C_TEAL;
        drawProgressBar(r, cx + 2, ry + 10, cprog - 8, 12, pct,
                        C_PANEL2, pfg, C_BORDER);
        char pctbuf[8]; std::snprintf(pctbuf, sizeof(pctbuf), "%d%%", (int)pct);
        int ptw = textW(f.tiny, pctbuf);
        renderText(r, f.tiny, pctbuf,
                   cx + 2 + (cprog - 8 - ptw) / 2, ry + 11, C_BG);

        if (job->preemption_count.load() > 0) {
            char pb[8];
            std::snprintf(pb, sizeof(pb), "x%d", job->preemption_count.load());
            renderText(r, f.tiny, pb, cx + cprog - 20, ry + 11, C_PINK);
        }

        fillRect(r, x + 4, ry + row_h - 2, w - 8, 1, C_BORDER);
        ry += row_h;
    }

    SDL_RenderSetClipRect(r, nullptr);

    if (total_h > clip_h && max_scr > 0) {
        int track_h = clip_h;
        int thumb_h = std::max(20, track_h * clip_h / total_h);
        int thumb_y = clip_y + (int)((float)ui.job_scroll / max_scr * (track_h - thumb_h));
        fillRect(r, x + w - 5, clip_y, 3, track_h, C_BORDER);
        fillRect(r, x + w - 5, thumb_y, 3, thumb_h, C_BORDER2);
    }
}

static void drawGanttPanel(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                            UIState& ui, int x, int y, int w, int h) {
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "GANTT TIMELINE", x + 10, y + 8, C_TEXT3);
    renderText(r, f.tiny,
        "color = MLFQ level  |  bar height = priority  |  pink marker = preempted",
        x + 140, y + 10, C_TEXT3);

    static GanttRecord gbuf[MAX_GANTT];
    int count = sched.ganttSnapshot(gbuf, MAX_GANTT);
    if (count == 0) {
        int mw = textW(f.medium, "No execution data yet — submit jobs to see the Gantt timeline");
        renderText(r, f.medium,
            "No execution data yet — submit jobs to see the Gantt timeline",
            x + (w - mw) / 2, y + h / 2 - 8, C_TEXT3);
        return;
    }

    int lx = x + w - 12;
    for (int l = MLFQ_LEVELS - 1; l >= 0; l--) {
        char lb[8]; std::snprintf(lb, sizeof(lb), "L%d", l);
        lx -= textW(f.tiny, lb) + 4;
        fillRect(r, lx - 16, y + 10, 12, 10, MLFQ_COL[l]);
        renderText(r, f.tiny, lb, lx, y + 9, MLFQ_COL[l]);
        lx -= 22;
    }

    int left_pad = 24;
    int row_h    = (h - 44) / MAX_CONCURRENT;
    int area_w   = w - left_pad - 10;
    int tick_w   = std::max(2, area_w / std::max(1, count));

    SDL_Rect clip{x + left_pad, y + 32, area_w, h - 44};
    SDL_RenderSetClipRect(r, &clip);

    for (int wk = 0; wk < MAX_CONCURRENT; wk++) {
        int wy = y + 32 + wk * row_h;
        fillRect(r, x + left_pad, wy, area_w, row_h - 2, C_PANEL2);
        char wlbl[8]; std::snprintf(wlbl, sizeof(wlbl), "W%d", wk);
        renderText(r, f.tiny, wlbl, x + 4, wy + row_h / 2 - 5, C_TEXT3);
        fillRect(r, x + left_pad, wy + row_h - 2, area_w, 1, C_BORDER);
    }

    int visible = area_w / std::max(2, tick_w);
    int start_i = std::max(0, count - visible - ui.gantt_scroll);
    int end_i   = std::min(count, start_i + visible);

    for (int i = start_i; i < end_i; i++) {
        const GanttRecord& g = gbuf[i];
        int wk = g.worker;
        if (wk < 0 || wk >= MAX_CONCURRENT) continue;

        int wy   = y + 32 + wk * row_h;
        int gx   = x + left_pad + (i - start_i) * tick_w;
        int pval = std::max(1, std::min(4, g.priority));
        int bar_h = (row_h - 4) * pval / 4;
        int bar_y = wy + (row_h - 4 - bar_h);

        int lvl = (g.mlfq_level >= 0 && g.mlfq_level < MLFQ_LEVELS) ? g.mlfq_level : 0;
        Color lc = MLFQ_COL[lvl];
        Color bc = {lc.r, lc.g, lc.b, g.preempted ? (Uint8)75 : (Uint8)170};
        fillRect(r, gx, bar_y, std::max(1, tick_w - 1), bar_h, bc);

        if (g.preempted)
            fillRect(r, gx, wy, tick_w, 2, C_PINK);
    }

    SDL_RenderSetClipRect(r, nullptr);

    int ruler_y = y + h - 14;
    fillRect(r, x + left_pad, ruler_y, area_w, 1, C_BORDER);
    if (visible > 0) {
        int step = std::max(1, visible / 12);
        for (int i = start_i; i < end_i; i += step) {
            int gx = x + left_pad + (i - start_i) * tick_w;
            char t[16]; std::snprintf(t, sizeof(t), "%d", i);
            renderText(r, f.tiny, t, gx, ruler_y + 2, C_TEXT3);
        }
    }
}

static void drawLogPanel(SDL_Renderer* r, Fonts& f,
                          UIState& ui, int x, int y, int w, int h) {
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "SYSTEM LOG", x + 10, y + 8, C_TEXT3);

    char cbuf[16];
    std::snprintf(cbuf, sizeof(cbuf), "%d entries", ui.log_count);
    renderText(r, f.tiny, cbuf, x + w - 10, y + 10, C_TEXT3, true);

    int clip_y = y + 26;
    int clip_h = h - 30;
    SDL_Rect clip{x + 6, clip_y, w - 12, clip_h};
    SDL_RenderSetClipRect(r, &clip);

    static UILogEntry entries[UI_LOG_MAX];
    int total = ui.logSnapshot(entries, UI_LOG_MAX);

    int line_h = 14;
    int vis    = clip_h / line_h;
    int max_s  = std::max(0, total - vis);
    int scroll = max_s - ui.log_scroll;
    if (scroll < 0) scroll = 0;
    if (scroll > total) scroll = total;

    Uint32 now_ms = SDL_GetTicks();
    int    ly     = clip_y;

    for (int i = scroll; i < total && ly < y + h - 4; i++) {
        const UILogEntry& e = entries[i];
        Uint32 age   = now_ms - e.born_ms;
        float  alpha = (age < 200) ? (age / 200.0f) : 1.0f;

        Color tc = LOG_COLS[e.color_hint < 6 ? e.color_hint : 0];
        tc.a = (Uint8)(alpha * 255);

        std::string line = fitText(f.mono, e.text, w - 20);
        renderText(r, f.mono, line.c_str(), x + 10, ly, tc);
        ly += line_h;
    }

    SDL_RenderSetClipRect(r, nullptr);

    if (total > vis && max_s > 0) {
        int thumb_h = std::max(16, clip_h * vis / total);
        int thumb_y = clip_y + (int)((float)(max_s - ui.log_scroll) / max_s * (clip_h - thumb_h));
        fillRect(r, x + w - 5, clip_y, 3, clip_h, C_BORDER);
        fillRect(r, x + w - 5, thumb_y, 3, thumb_h, C_BORDER2);
    }
}

static int drawTabBar(SDL_Renderer* r, Fonts& f,
                       int x, int y, int w, UIState& ui,
                       const SDL_Event* ev) {
    const char* tabs[] = {"JOB TABLE", "GANTT TIMELINE", "STATISTICS"};
    int tw = (w - 2 * 5) / 3;
    for (int i = 0; i < 3; i++) {
        int tx  = x + i * (tw + 5);
        bool sel = (ui.active_tab == i);
        fillRoundRect(r, tx, y, tw, 28, 4, sel ? C_PANEL3 : C_PANEL2);
        drawRect(r, tx, y, tw, 28, sel ? C_TEAL : C_BORDER);
        if (sel) fillRect(r, tx, y + 26, tw, 2, C_TEAL);
        int lw = textW(f.small, tabs[i]);
        renderText(r, f.small, tabs[i], tx + (tw - lw) / 2, y + 7,
                   sel ? C_TEAL : C_TEXT3);
        if (ev && ev->type == SDL_MOUSEBUTTONDOWN) {
            int mx = ev->button.x, my = ev->button.y;
            if (mx >= tx && mx < tx+tw && my >= y && my < y+28)
                ui.active_tab = i;
        }
    }
    return y + 32;
}

static void drawStatsDash(SDL_Renderer* r, Fonts& f, Scheduler& sched,
                           int x, int y, int w, int h) {
    fillRoundRect(r, x, y, w, h, 8, C_PANEL);
    drawRect(r, x, y, w, h, C_BORDER);
    renderText(r, f.small, "SCHEDULING STATISTICS", x + 10, y + 8, C_TEXT3);

    auto& s = sched.stats();

    char vs[5][24];
    std::snprintf(vs[0], sizeof(vs[0]), "%d", s.total_submitted.load());
    std::snprintf(vs[1], sizeof(vs[1]), "%d", s.total_completed.load());
    std::snprintf(vs[2], sizeof(vs[2]), "%d", s.total_failed.load());
    std::snprintf(vs[3], sizeof(vs[3]), "%d", s.total_preemptions.load());
    std::snprintf(vs[4], sizeof(vs[4]), "%d", s.total_aging_promotions.load());

    struct Card { const char* val_str; const char* lbl; Color col; };
    const Card cards[] = {
        {vs[0], "SUBMITTED",   C_TEXT2  },
        {vs[1], "COMPLETED",   C_GREEN  },
        {vs[2], "FAILED",      C_CORAL  },
        {vs[3], "PREEMPTIONS", C_PINK   },
        {vs[4], "PROMOTIONS",  C_PURPLE },
    };

    int bw = (w - 20) / 5;
    for (int i = 0; i < 5; i++) {
        int bx = x + 10 + i * bw;
        fillRoundRect(r, bx, y + 28, bw - 6, 58, 6, C_PANEL2);
        drawRect(r, bx, y + 28, bw - 6, 58, cards[i].col);
        fillRect(r, bx, y + 28, bw - 6, 3, cards[i].col);
        int vw = textW(f.large, cards[i].val_str);
        renderText(r, f.large, cards[i].val_str,
                   bx + (bw - 6 - vw) / 2, y + 36, cards[i].col);
        int lw2 = textW(f.tiny, cards[i].lbl);
        renderText(r, f.tiny, cards[i].lbl,
                   bx + (bw - 6 - lw2) / 2, y + 62, C_TEXT3);
    }

    int gy = y + 100;
    fillRect(r, x + 6, gy, w - 12, 1, C_BORDER);
    gy += 10;

    char buf[48];
    auto avg_stat = [&](const char* lbl, const char* val, Color vc, int ox) {
        renderText(r, f.small, lbl, x + 10 + ox, gy, C_TEXT3);
        renderText(r, f.medium, val, x + 10 + ox, gy + 16, vc);
    };

    int third = (w - 20) / 3;
    std::snprintf(buf, sizeof(buf), "%.1f ticks", s.avg_wait_ticks.load());
    avg_stat("AVG WAIT TIME",  buf, C_AMBER, 0);
    std::snprintf(buf, sizeof(buf), "%.1f sec", s.avg_turnaround_sec.load());
    avg_stat("AVG TURNAROUND", buf, C_BLUE,  third);
    std::snprintf(buf, sizeof(buf), "%.2f j/s", s.throughput.load());
    avg_stat("THROUGHPUT",     buf, C_TEAL,  2 * third);

    gy += 48;
    fillRect(r, x + 6, gy, w - 12, 1, C_BORDER);
    gy += 10;
    renderText(r, f.small, "CPU UTILIZATION", x + 10, gy, C_TEXT3);
    std::snprintf(buf, sizeof(buf), "%.1f%%", s.cpu_utilization.load());
    renderText(r, f.medium, buf, x + w - 10, gy, C_TEAL, true);
    gy += 18;
    drawProgressBar(r, x + 10, gy, w - 20, 12, s.cpu_utilization.load(),
                    C_PANEL2, C_TEAL, C_BORDER, true);

    gy += 22;
    fillRect(r, x + 6, gy, w - 12, 1, C_BORDER);
    gy += 10;
    renderText(r, f.small, "SCHEDULING ALGORITHM", x + 10, gy, C_TEXT3);
    gy += 18;

    struct AlgoLine { const char* text; Color col; };
    const AlgoLine algo[] = {
        {"1. All new jobs enter MLFQ Level 0 (quantum=2t, most responsive)", C_TEAL   },
        {"2. Within each level: Priority-Queue (CRITICAL > HIGH > MEDIUM > LOW)", C_AMBER  },
        {"3. Equal-priority jobs at same level: Round-Robin (FIFO by submit time)", C_BLUE   },
        {"4. Job uses full quantum → demoted to next MLFQ level (quantum doubles)", C_PURPLE },
        {"5. Higher-priority job arrives → lower-priority job is preempted (CAS)", C_PINK   },
        {"6. Job waiting > threshold at L1/L2 → promoted to L0 (aging thread)", C_GREEN  },
        {"7. Semaphore limits concurrent executions to MAX_CONCURRENT workers", C_AMBER  },
        {"8. Mutex locks protect queue, job list, log, and Gantt ring buffers", C_BLUE   },
    };

    for (const AlgoLine& al : algo) {
        fillRoundRect(r, x + 10, gy, 4, 12, 2, al.col);
        renderText(r, f.tiny, al.text, x + 20, gy, C_TEXT2);
        gy += 17;
        if (gy > y + h - 10) break;
    }
}

int main(int /*argc*/, char* /*argv*/[]) {
    std::srand((unsigned)std::time(nullptr));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL_Init error: " << SDL_GetError() << "\n";
        return 1;
    }
    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init error: " << TTF_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "TensorSched v3.0  —  MLFQ + Priority-Queue + Round-Robin",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow error: " << SDL_GetError() << "\n";
        TTF_Quit(); SDL_Quit(); return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer error: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window); TTF_Quit(); SDL_Quit(); return 1;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    Fonts fonts;
    if (!fonts.loadSystem()) {
        std::cerr << "Warning: Could not load any font. Text will be absent.\n";
    }

    UIState   ui;
    FormState form;
    ui.start_ms = SDL_GetTicks();

    auto log_cb = [&](const char* msg, int hint) {
        ui.pushLog(msg, hint);
    };
    Scheduler sched(log_cb, nullptr);
    sched.start();

    ui.pushLog("[TENSORSCHED] System online. MLFQ + Priority-Queue + Round-Robin active.", 5);
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
            "[TENSORSCHED] L0=%dt | L1=%dt | L2=%dt | Aging=%dt | Workers=%d",
            MLFQ_QUANTUM[0], MLFQ_QUANTUM[1], MLFQ_QUANTUM[2],
            AGING_THRESHOLD, MAX_CONCURRENT);
        ui.pushLog(msg, 5);
    }
    ui.pushLog("[TENSORSCHED] Use QUICK SUBMIT PRESETS or fill the form to add jobs.", 5);

    bool   quit       = false;
    Uint32 last_frame = SDL_GetTicks();

    while (!quit) {
        SDL_Event ev;
        bool      has_ev  = false;
        SDL_Event ev_copy{};

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;

            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) quit = true;

                if (form.focused == 0) {
                    if (ev.key.keysym.sym == SDLK_BACKSPACE) {
                        int len = (int)std::strlen(form.name_buf);
                        if (len > 0) form.name_buf[len - 1] = '\0';
                    } else if (ev.key.keysym.sym == SDLK_TAB ||
                               ev.key.keysym.sym == SDLK_RETURN) {
                        form.focused = -1;
                    }
                }
            }

            if (ev.type == SDL_TEXTINPUT && form.focused == 0) {
                int len = (int)std::strlen(form.name_buf);
                if (len < 58 && ev.text.text[0] >= ' ') {
                    form.name_buf[len]     = ev.text.text[0];
                    form.name_buf[len + 1] = '\0';
                }
            }

            if (ev.type == SDL_MOUSEWHEEL) {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                if (mx > SB_W + PAD) {
                    if (ui.active_tab == 0)
                        ui.job_scroll = std::max(0, ui.job_scroll - ev.wheel.y * 34);
                    else if (ui.active_tab == 1)
                        ui.gantt_scroll = std::max(0, ui.gantt_scroll + ev.wheel.y * 5);
                }
                if (my > WIN_H - 180)
                    ui.log_scroll = std::max(0, ui.log_scroll + ev.wheel.y * 3);
            }

            has_ev  = true;
            ev_copy = ev;
        }

        Uint32 now_ms = SDL_GetTicks();
        if (now_ms - ui.last_spark_ms > 350) {
            ui.cpu_spark.push((float)sched.resources().cpuPct());
            ui.mem_spark.push((float)sched.resources().memoryPct());
            for (int l = 0; l < MLFQ_LEVELS; l++)
                ui.mlfq_spark[l].push((float)sched.queue().sizeAtLevel(l));
            ui.last_spark_ms = now_ms;
        }

        setColor(renderer, C_BG);
        SDL_RenderClear(renderer);

        const SDL_Event* evp = has_ev ? &ev_copy : nullptr;

        drawHeader(renderer, fonts, sched, ui);

        int top  = HDR_H + PAD;
        int sb_x = PAD;
        int sb_y = top;

        drawResourcePanel(renderer, fonts, sched, ui, sb_x, sb_y, SB_W, 240);
        sb_y += 244;

        drawMLFQPanel(renderer, fonts, sched, ui, sb_x, sb_y, SB_W, 216);
        sb_y += 220;

        drawConceptsPanel(renderer, fonts, sched, sb_x, sb_y, SB_W, 256);
        sb_y += 260;

        int form_h = WIN_H - sb_y - PAD;
        if (form_h > 80)
            drawFormPanel(renderer, fonts, form, sched, sb_x, sb_y, SB_W, form_h, evp);

        int mx2 = SB_W + PAD * 2;
        int mw  = WIN_W - mx2 - PAD;
        int my2 = top;

        drawWorkerSlots(renderer, fonts, sched, mx2, my2, mw);
        my2 += 90;

        drawPresets(renderer, fonts, sched, mx2, my2 + 16, mw, evp);
        my2 += 64;

        my2 = drawTabBar(renderer, fonts, mx2, my2, mw, ui, evp);

        int tab_h = WIN_H - my2 - 168;
        if (tab_h < 80) tab_h = 80;

        if (ui.active_tab == 0)
            drawJobTable(renderer, fonts, sched, ui, mx2, my2, mw, tab_h);
        else if (ui.active_tab == 1)
            drawGanttPanel(renderer, fonts, sched, ui, mx2, my2, mw, tab_h);
        else
            drawStatsDash(renderer, fonts, sched, mx2, my2, mw, tab_h);

        drawLogPanel(renderer, fonts, ui, mx2, WIN_H - 162, mw, 156);

        SDL_RenderPresent(renderer);

        Uint32 elapsed = SDL_GetTicks() - last_frame;
        if (elapsed < 1000u / FPS)
            SDL_Delay(1000u / FPS - elapsed);
        last_frame = SDL_GetTicks();
    }

    sched.stop();
    fonts.close();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}