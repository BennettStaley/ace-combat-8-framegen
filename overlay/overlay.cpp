/*
 * AC8 Tweaks in-game menu.
 *
 * A DLL the Lua core loads with package.loadlib. It draws a Dear ImGui menu on every frame the
 * game presents, generated frames included, by patching Present in the swap chain vtable of the
 * system dxgi.dll. That is the swap chain underneath Streamline, so the menu is on real and
 * generated frames alike.
 *
 * The DLL knows nothing about the game. The Lua core describes the menu in AC8Tweaks\menu.txt
 * (sections and rows, one per line, tab separated) and this file reports what the player did in
 * AC8Tweaks\menu-events.txt. Lua acknowledges events by sequence number in the next menu.txt.
 *
 * ImGui is drawn into an 8-bit texture of our own and then copied onto the back buffer by one
 * small shader, which is where SDR colours are converted for HDR10 and scRGB back buffers.
 *
 * F10 opens and closes the menu, or whichever key Lua names. While it is open, ImGui's own Win32
 * backend reads the game window's keyboard and mouse messages, and the ones ImGui wants never
 * reach the game. Lua can also list quick keys to watch while the menu is closed; pressing one is
 * reported like any other event, and Lua may answer with a line of text to show for a moment.
 *
 * Nothing is created and nothing is drawn while the menu is closed and no such text is up.
 * Refuses to start unless the offline launcher started the game (AC8TWEAKS_OFFLINE=1).
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

/* ------------------------------------------------------------------------------------------ */
/* What Lua tells us                                                                           */
/* ------------------------------------------------------------------------------------------ */

struct Row {
    std::string id, kind, label, text;
    double value = 0, step = 1, lo = 0, hi = 0;
    bool has_lo = false, has_hi = false;
    int decimals = 0;
    std::vector<std::string> choices;
};

struct Section {
    std::string id, label;
    bool open = true;
    std::vector<Row> rows;
};

/* A key as one number: the Windows key code, plus 256 if CTRL has to be held, 512 for SHIFT, 1024 for ALT. */
struct Hotkey { std::string id; int code; };

struct Menu {
    std::string title = "AC8 Tweaks", note, toast, stats;
    int hdr = 0;
    float nits = 200.0f;
    unsigned long long ack = 0;
    int key = VK_F10; /* opens and closes the menu */
    std::string key_name = "F10";
    std::vector<Hotkey> hotkeys; /* watched while the menu is closed */
    std::vector<Section> sections;
};

static std::mutex g_lock; /* guards g_menu, g_events, g_events_dirty */
static std::shared_ptr<const Menu> g_menu = std::make_shared<Menu>();
static std::vector<std::pair<unsigned long long, std::string>> g_events;
static unsigned long long g_seq = 0;
static bool g_events_dirty = false;
static std::string g_status_text; /* what the menu can see right now, written to overlay-status.txt by the worker */
static bool g_status_dirty = false;

static std::wstring g_dir; /* <game root>\AC8Tweaks\ */
static bool g_test = false; /* harness: keys come from ac8overlay_test_key, focus is not checked */
static bool g_test_keys[256];
static volatile bool g_dead = false; /* set on any failure; the menu then stays away for good */

static void logf(const char *fmt, ...)
{
    static std::mutex m;
    std::lock_guard<std::mutex> hold(m);
    FILE *f = _wfopen((g_dir + L"overlay.log").c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t next = s.find(sep, pos);
        out.push_back(s.substr(pos, next == std::string::npos ? next : next - pos));
        if (next == std::string::npos) return out;
        pos = next + 1;
    }
}

static std::shared_ptr<Menu> parse_menu(const std::string &text)
{
    auto menu = std::make_shared<Menu>();
    for (std::string &line : split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f = split(line, '\t');
        const std::string &tag = f[0];
        auto field = [&](size_t i) -> const std::string & { static const std::string none; return i < f.size() ? f[i] : none; };
        if (tag == "hdr") menu->hdr = atoi(field(1).c_str());
        else if (tag == "nits") menu->nits = (float)atof(field(1).c_str());
        else if (tag == "ack") menu->ack = strtoull(field(1).c_str(), nullptr, 10);
        else if (tag == "title") menu->title = field(1);
        else if (tag == "note") menu->note = field(1);
        else if (tag == "toast") menu->toast = field(1);
        else if (tag == "stats") menu->stats = field(1);
        else if (tag == "key" || tag == "hotkey") {
            bool quick = tag == "hotkey";
            int code = atoi(field(quick ? 2 : 1).c_str());
            if ((code & 255) == 0 || code < 0 || code >= 2048) continue;
            if (quick) menu->hotkeys.push_back({ field(1), code });
            else {
                menu->key = code;
                if (!field(2).empty()) menu->key_name = field(2);
            }
        } else if (tag == "section") {
            Section s;
            s.id = field(1);
            s.label = field(2);
            s.open = field(3) != "0";
            menu->sections.push_back(s);
        } else if (tag == "row" && !menu->sections.empty()) {
            Row r;
            r.id = field(1);
            r.kind = field(2);
            r.label = field(3);
            if (r.kind == "switch") r.value = atof(field(4).c_str());
            else if (r.kind == "number") {
                r.value = atof(field(4).c_str());
                r.step = atof(field(5).c_str());
                r.has_lo = !field(6).empty();
                r.has_hi = !field(7).empty();
                r.lo = atof(field(6).c_str());
                r.hi = atof(field(7).c_str());
                r.decimals = std::clamp(atoi(field(8).c_str()), 0, 6);
            } else if (r.kind == "choice") {
                r.value = atof(field(4).c_str());
                r.choices = split(field(5), '|');
            } else r.text = field(4); /* for a key row, the name of the key it is set to */
            menu->sections.back().rows.push_back(r);
        }
    }
    return menu;
}

static void push_event(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> hold(g_lock);
    g_events.emplace_back(++g_seq, buf);
    g_events_dirty = true;
}

/* Reads menu.txt when it changes and keeps menu-events.txt equal to the events Lua has not acknowledged. */
[[noreturn]] static void worker_loop()
{
    std::wstring menu_path = g_dir + L"menu.txt", events_path = g_dir + L"menu-events.txt", tmp_path = events_path + L".tmp";
    FILETIME seen = {};
    DeleteFileW(events_path.c_str());
    for (;;) {
        Sleep(g_test ? 5 : 40);
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(menu_path.c_str(), GetFileExInfoStandard, &info) && CompareFileTime(&info.ftLastWriteTime, &seen) != 0) {
            HANDLE h = CreateFileW(menu_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                std::string text(GetFileSize(h, nullptr), '\0');
                DWORD got = 0;
                BOOL ok = text.empty() || ReadFile(h, &text[0], (DWORD)text.size(), &got, nullptr);
                CloseHandle(h);
                if (ok && got == text.size()) {
                    seen = info.ftLastWriteTime;
                    std::shared_ptr<Menu> menu = parse_menu(text);
                    std::lock_guard<std::mutex> hold(g_lock);
                    size_t before = g_events.size();
                    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](auto &e) { return e.first <= menu->ack; }), g_events.end());
                    if (g_events.size() != before) g_events_dirty = true;
                    g_menu = menu;
                }
            }
        }
        std::string out, status;
        bool dirty;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (g_status_dirty) status = g_status_text;
            g_status_dirty = false;
            dirty = g_events_dirty;
            g_events_dirty = false;
            if (dirty)
                for (auto &e : g_events) out += std::to_string(e.first) + "\t" + e.second + "\n";
        }
        if (!status.empty()) {
            FILE *f = _wfopen((g_dir + L"overlay-status.txt").c_str(), L"wb");
            if (f) {
                fwrite(status.data(), 1, status.size(), f);
                fclose(f);
            }
        }
        if (!dirty) continue;
        if (out.empty()) {
            DeleteFileW(events_path.c_str());
            continue;
        }
        HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD put = 0;
        bool written = h != INVALID_HANDLE_VALUE && WriteFile(h, out.data(), (DWORD)out.size(), &put, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!written || !MoveFileExW(tmp_path.c_str(), events_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::lock_guard<std::mutex> hold(g_lock); /* Lua had the file open; try again next round */
            g_events_dirty = true;
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Keyboard and mouse                                                                          */
/* ------------------------------------------------------------------------------------------ */

static HWND g_hwnd;
static double g_now; /* seconds, set once per presented frame */
static bool g_open = false;
static bool g_ready;            /* ImGui and the D3D12 objects exist */
/* ImGui is fed on the game's window thread and drawn on the presenting thread. Recursive, because
   ImGui's handler makes calls (SetCapture, ReleaseCapture) that send the same window another
   message before they return, and that message comes straight back into menu_wndproc. */
static std::recursive_mutex g_imgui_lock;

/* true while a window of the game itself has the keyboard */
static bool game_in_front()
{
    if (g_test) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

/* Whoever asks has checked game_in_front() first: keys pressed in another program are not ours. */
static bool key_down(int vk)
{
    if (g_test) return g_test_keys[vk & 255];
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* CTRL, SHIFT and ALT as held right now, in the bits a key number carries above the key code. */
static int held_modifiers()
{
    return (key_down(VK_CONTROL) ? 1 : 0) | (key_down(VK_SHIFT) ? 2 : 0) | (key_down(VK_MENU) ? 4 : 0);
}

static int g_settle; /* a key just picked in the menu: it does nothing until it has been let go */

/*
 * The key Lua listed that went down on this frame: -1 for the menu key, the index of a quick key,
 * -2 for none. A key set with CTRL, SHIFT or ALT needs those held. Other modifiers held at the
 * time do not matter, since in flight some usually are; when two keys differ only in modifiers,
 * the one asking for more of them wins.
 */
static int key_pressed(const Menu &m)
{
    static bool was[256];
    bool down[256] = {}, seen[256] = {}, front = game_in_front();
    if (g_settle && !(front && key_down(g_settle))) g_settle = 0;
    int held = -1, best = -2, best_count = -1;
    for (int i = -1; i < (int)m.hotkeys.size(); i++) {
        int code = i < 0 ? m.key : m.hotkeys[i].code, vk = code & 255, mods = code >> 8;
        if (!seen[vk]) {
            seen[vk] = true;
            down[vk] = front && key_down(vk);
        }
        if (!down[vk] || was[vk] || vk == g_settle) continue;
        if (held < 0) held = held_modifiers();
        int count = (mods & 1) + (mods >> 1 & 1) + (mods >> 2 & 1);
        if ((mods & held) == mods && count > best_count) {
            best = i;
            best_count = count;
        }
    }
    for (int vk = 0; vk < 256; vk++) was[vk] = seen[vk] && down[vk];
    return best;
}

/* For picking a key in the menu: the key that went down since the last call, 0 for none. CTRL, SHIFT, ALT and the mouse do not count. */
static int fresh_key()
{
    static bool was[256];
    int hit = 0;
    bool front = game_in_front();
    for (int vk = 8; vk < 255; vk++) {
        if ((vk >= VK_SHIFT && vk <= VK_MENU) || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN) continue;
        bool down = front && key_down(vk);
        if (down && !was[vk] && !hit) hit = vk;
        was[vk] = down;
    }
    return hit;
}

static WNDPROC g_game_wndproc;
static HWND g_subclassed;
static std::string g_capture;     /* the key row waiting for a key to be pressed, empty for none */
static volatile bool g_capturing; /* the same, for the window procedure */

/*
 * Sits in front of the game's own window procedure. While the menu is open every message is
 * shown to ImGui, and key presses, typed characters and mouse input that ImGui wants (the
 * pointer is over the menu, or the menu has the keyboard) stop here. Key releases always go
 * through, so a key held when the menu opened never sticks in the game. A gamepad is untouched.
 * A key pressed while the menu waits for one to be picked is for neither ImGui nor the game.
 */
static LRESULT CALLBACK menu_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_open && g_ready && !g_dead) {
        if (g_capturing && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR || msg == WM_SYSCHAR)) return 0;
        bool wants_mouse = false, wants_keys = false;
        try {
            std::lock_guard<std::recursive_mutex> hold(g_imgui_lock);
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            wants_mouse = ImGui::GetIO().WantCaptureMouse;
            wants_keys = ImGui::GetIO().WantCaptureKeyboard;
        } catch (...) { /* an exception escaping a window procedure ends the whole game */
            g_dead = true;
            logf("failed while reading input, the menu is off until the game restarts");
        }
        if (wants_mouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
        if (wants_mouse && msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp); /* raw mouse movement */
        if (wants_keys && (msg == WM_KEYDOWN || msg == WM_CHAR)) return 0;
    }
    return CallWindowProcW(g_game_wndproc, hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------------------------------ */
/* The menu itself                                                                             */
/* ------------------------------------------------------------------------------------------ */

struct Pending { double value; unsigned long long seq; };

static std::map<std::string, bool> g_section_open; /* as last told to Lua */
static std::map<std::string, Pending> g_pending;   /* values changed here that Lua has not confirmed yet */
static std::map<std::string, double> g_editing;    /* a number being typed or stepped, not sent yet */
static char g_status[160];
static float g_scale = 1.0f;
static std::string g_ini_path;

static double clamp_row(const Row &r, double v)
{
    if (r.has_lo && v < r.lo) v = r.lo;
    if (r.has_hi && v > r.hi) v = r.hi;
    return v;
}

static void set_value(const Row &r, double v)
{
    push_event("set\t%s\t%.17g", r.id.c_str(), v);
    g_pending[r.id] = { v, g_seq };
}

static void draw_row(const Menu &m, const Row &r)
{
    ImGui::PushID(r.id.c_str());
    double value = r.value;
    auto p = g_pending.find(r.id);
    if (p != g_pending.end()) {
        if (m.ack >= p->second.seq) g_pending.erase(p);
        else value = p->second.value;
    }
    if (r.kind == "switch") {
        bool on = value != 0;
        if (ImGui::Checkbox(r.label.c_str(), &on)) set_value(r, on ? 1 : 0);
    } else if (r.kind == "number") {
        /* The box starts with the current value selected. Nothing is sent until the edit ends. */
        auto e = g_editing.find(r.id);
        double v = e != g_editing.end() ? e->second : value;
        char format[8];
        snprintf(format, sizeof format, "%%.%df", r.decimals);
        if (ImGui::InputDouble(r.label.c_str(), &v, r.step, r.step * 10, format)) g_editing[r.id] = clamp_row(r, v);
        if (ImGui::IsItemDeactivated()) {
            e = g_editing.find(r.id);
            if (e != g_editing.end()) {
                if (ImGui::IsItemDeactivatedAfterEdit() && !ImGui::IsKeyPressed(ImGuiKey_Escape)) set_value(r, e->second);
                g_editing.erase(e);
            }
        }
    } else if (r.kind == "choice" && !r.choices.empty()) {
        int current = std::clamp((int)value, 0, (int)r.choices.size() - 1);
        if (ImGui::BeginCombo(r.label.c_str(), r.choices[current].c_str())) {
            for (int i = 0; i < (int)r.choices.size(); i++) {
                if (ImGui::Selectable(r.choices[i].c_str(), i == current)) set_value(r, i);
                if (i == current) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else if (r.kind == "key") {
        /* A button showing the key. Pressed, it waits for the next key: Esc keeps the old one, Backspace sets none. */
        bool waiting = g_capture == r.id;
        std::string face = (waiting ? "press a key" : r.text.empty() ? "not set" : r.text) + "###key";
        if (ImGui::Button(face.c_str(), ImVec2(ImGui::CalcItemWidth(), 0))) {
            waiting = !waiting;
            g_capture = waiting ? r.id : "";
            fresh_key(); /* whatever is held right now is not the answer */
        }
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextUnformatted(r.label.c_str());
        if (waiting) {
            int vk = fresh_key();
            if (vk == VK_BACK) push_event("set\t%s\t0", r.id.c_str());
            else if (vk && vk != VK_ESCAPE) push_event("set\t%s\t%d", r.id.c_str(), vk | held_modifiers() << 8);
            if (vk) {
                g_capture.clear();
                g_settle = vk;
            }
        }
    } else if (r.kind == "action") {
        if (ImGui::Button(r.label.c_str())) push_event("press\t%s", r.id.c_str());
        if (!r.text.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", r.text.c_str());
        }
    } else {
        ImGui::LabelText(r.label.c_str(), "%s", r.text.c_str());
    }
    ImGui::PopID();
}

static void draw_menu(const Menu &m, float width, float height)
{
    ImGui::SetNextWindowPos(ImVec2(width * 0.03f, height * 0.08f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f * g_scale, height * 0.70f), ImGuiCond_FirstUseEver);
    bool keep_open = true;
    if (ImGui::Begin((m.title + "###ac8tweaks").c_str(), &keep_open)) {
        ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.38f); /* leaves room for the labels, which ImGui puts on the right */
        if (!m.note.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            ImGui::TextWrapped("%s", m.note.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
        bool capture_shown = false; /* the row waiting for a key may have gone: its group closed, or Lua dropped it */
        for (const Section &s : m.sections) {
            ImGui::SetNextItemOpen(s.open, ImGuiCond_Once);
            bool open = ImGui::CollapsingHeader((s.label + "###" + s.id).c_str());
            auto known = g_section_open.find(s.id);
            if (open != (known == g_section_open.end() ? s.open : known->second)) {
                g_section_open[s.id] = open;
                push_event("section\t%s\t%d", s.id.c_str(), open ? 1 : 0); /* Lua only fills in groups that are open */
            }
            if (!open) continue;
            ImGui::PushID(s.id.c_str());
            for (const Row &r : s.rows) {
                draw_row(m, r);
                capture_shown = capture_shown || (r.kind == "key" && r.id == g_capture);
            }
            ImGui::PopID();
        }
        if (!capture_shown) g_capture.clear();
        ImGui::Separator();
        if (!g_capture.empty()) ImGui::TextDisabled("Press the key to use. Esc keeps the old one, Backspace sets none.");
        else ImGui::TextDisabled("%s closes. Mouse, or arrow keys with Space and Enter.", m.key_name.c_str());
        ImGui::TextDisabled("%s", g_status);
        ImGui::PopItemWidth();
    } else g_capture.clear(); /* collapsed to its title bar */
    ImGui::End();
    if (!keep_open) g_open = false; /* the X in the title bar */
    g_capturing = !g_capture.empty();
}

/* What a quick key just did, on its own in the corner while the menu is closed. */
static void draw_toast(const Menu &m, float width, float height)
{
    ImGui::SetNextWindowPos(ImVec2(width * 0.03f, height * 0.08f));
    ImGui::SetNextWindowBgAlpha(0.80f);
    if (ImGui::Begin("###ac8toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
        ImGui::TextUnformatted(m.toast.c_str());
    ImGui::End();
}

/* A small always-on readout in the top-right corner: render and generated frame rate, the DLSS model. */
static void draw_stats(const Menu &m, float width, float height)
{
    ImGui::SetNextWindowPos(ImVec2(width * 0.985f, height * 0.025f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("###ac8stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
        ImGui::TextUnformatted(m.stats.c_str());
    ImGui::End();
}

/* ------------------------------------------------------------------------------------------ */
/* Direct3D 12                                                                                 */
/* ------------------------------------------------------------------------------------------ */

template <typename T> static void release(T *&p)
{
    if (p) p->Release();
    p = nullptr;
}

struct FrameContext { ID3D12CommandAllocator *allocator; UINT64 fence_value; };

static ID3D12Device *g_device;
static ID3D12CommandQueue *g_queue;
static ID3D12DescriptorHeap *g_srv_heap, *g_rtv_heap;
static ID3D12GraphicsCommandList *g_list;
static ID3D12Fence *g_fence;
static HANDLE g_fence_event;
static UINT64 g_fence_value;
static FrameContext g_frames[3];
static UINT g_frame;
static ID3D12Resource *g_canvas; /* what ImGui draws into */
static UINT g_canvas_w, g_canvas_h;
static ID3D12RootSignature *g_root;
static ID3D12PipelineState *g_pso;
static DXGI_FORMAT g_pso_format;
static UINT g_srv_size, g_rtv_size;
static bool g_srv_used[16];

static const char *SHADER = R"(
cbuffer P : register(b0) { float mode; float scale; };
Texture2D canvas : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 to_linear(float3 c) { return lerp(c / 12.92, pow((abs(c) + 0.055) / 1.055, 2.4), step(0.04045, c)); }
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float4 c = canvas.Load(int3(pos.xy, 0));
    if (c.a <= 0) discard;
    float3 rgb = c.rgb / c.a;                 // the canvas holds premultiplied colour
    if (mode == 1) {                          // scRGB: linear, 1.0 is 80 nits
        rgb = to_linear(rgb) * scale;
    } else if (mode == 2) {                   // HDR10: Rec.2020 primaries, PQ curve, 1.0 is 10000 nits
        float3 l = to_linear(rgb);
        float3 w = float3(dot(l, float3(0.6274, 0.3293, 0.0433)), dot(l, float3(0.0691, 0.9195, 0.0114)), dot(l, float3(0.0164, 0.0880, 0.8956))) * scale;
        float3 p = pow(abs(w), 0.1593017578125);
        rgb = pow((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375);
    }
    return float4(rgb * c.a, c.a);
}
)";

static void srv_alloc(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
    for (UINT i = 1; i < 16; i++) { /* slot 0 is the canvas */
        if (g_srv_used[i]) continue;
        g_srv_used[i] = true;
        cpu->ptr = g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)i * g_srv_size;
        gpu->ptr = g_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)i * g_srv_size;
        return;
    }
    cpu->ptr = 0;
    gpu->ptr = 0;
}

static void srv_free(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    SIZE_T i = (cpu.ptr - g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srv_size;
    if (i < 16) g_srv_used[i] = false;
}

static void wait_for(UINT64 value)
{
    if (g_fence->GetCompletedValue() >= value) return;
    g_fence->SetEventOnCompletion(value, g_fence_event);
    WaitForSingleObject(g_fence_event, 2000);
}

/*
 * DXGI has no call that returns the command queue a swap chain presents on, and drawing on any
 * other queue would race the game's own frame. install_hooks() learns where a swap chain keeps
 * that pointer from a throwaway swap chain made with a queue of our own; the same places are
 * read here. A candidate only counts if it has the vtable every D3D12 queue shares.
 */
static std::vector<size_t> g_queue_offsets;
static void *g_queue_vtable;

static void *queue_at(void *swap_chain, size_t offset)
{
    __try {
        void *p = *(void **)((char *)swap_chain + offset);
        if (p && *(void **)p == g_queue_vtable) return p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

static bool create_device_objects(IDXGISwapChain *sc)
{
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) { logf("the swap chain is not a D3D12 one"); return false; }
    for (size_t offset : g_queue_offsets) {
        ID3D12CommandQueue *queue = (ID3D12CommandQueue *)queue_at(sc, offset);
        if (!queue) continue;
        ID3D12Device *owner = nullptr;
        bool same = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&owner))) && owner == g_device;
        release(owner);
        if (!same || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) continue;
        queue->AddRef();
        g_queue = queue;
        logf("present queue found at swap chain +0x%zx", offset);
        break;
    }
    if (!g_queue) { logf("could not find the queue the swap chain presents on"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 16;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_srv_heap)))) return false;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 2; /* 0 back buffer, 1 canvas */
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    g_srv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rtv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (FrameContext &f : g_frames)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_list)))) return false;
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 2;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &range };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root = { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = nullptr, *error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) return false;
    HRESULT hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root));
    blob->Release();
    if (FAILED(hr)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    g_ini_path.resize(g_dir.size() * 3 + 16);
    g_ini_path.resize(WideCharToMultiByte(CP_UTF8, 0, (g_dir + L"imgui.ini").c_str(), -1, &g_ini_path[0], (int)g_ini_path.size(), nullptr, nullptr));
    io.IniFilename = g_ini_path.c_str(); /* remembers where the window was put and how big */
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    DXGI_SWAP_CHAIN_DESC desc;
    sc->GetDesc(&desc);
    g_scale = std::clamp(desc.BufferDesc.Height / 1080.0f, 1.0f, 2.5f);
    wchar_t windir[MAX_PATH];
    char font[MAX_PATH * 2] = "";
    if (GetWindowsDirectoryW(windir, MAX_PATH)) WideCharToMultiByte(CP_UTF8, 0, (std::wstring(windir) + L"\\Fonts\\segoeui.ttf").c_str(), -1, font, sizeof font, nullptr, nullptr);
    if (!font[0] || GetFileAttributesA(font) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(font, 18.0f * g_scale)) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1.4f * g_scale;
    }
    /* ImGui's own dark theme, a little rounder, with the game's HUD green as the one accent. */
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = style.GrabRounding = style.PopupRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.ScaleAllSizes(g_scale);
    const ImVec4 green(0.33f, 0.86f, 0.55f, 1.0f), green_dim(0.20f, 0.52f, 0.34f, 1.0f), slate(0.17f, 0.20f, 0.22f, 1.0f), slate_lit(0.24f, 0.29f, 0.31f, 1.0f);
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.08f, 0.94f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.07f, 0.08f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.19f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Header] = c[ImGuiCol_Button] = slate;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered] = slate_lit;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive] = green_dim;
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_NavHighlight] = green;
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = c[ImGuiCol_TextSelectedBg] = green_dim;
    c[ImGuiCol_ResizeGrip] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = slate_lit;

    if (!ImGui_ImplWin32_Init(desc.OutputWindow)) return false;
    g_game_wndproc = (WNDPROC)SetWindowLongPtrW(desc.OutputWindow, GWLP_WNDPROC, (LONG_PTR)menu_wndproc);
    g_subclassed = desc.OutputWindow;
    if (!g_game_wndproc) { logf("could not read the game window's messages"); return false; }

    ImGui_ImplDX12_InitInfo info;
    info.Device = g_device;
    info.CommandQueue = g_queue;
    info.NumFramesInFlight = 3;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.SrvDescriptorHeap = g_srv_heap;
    info.SrvDescriptorAllocFn = srv_alloc;
    info.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&info)) return false;
    return true;
}

static bool ensure_canvas(UINT w, UINT h)
{
    if (g_canvas && g_canvas_w == w && g_canvas_h == h) return true;
    wait_for(g_fence_value); /* nothing on the GPU may still be using the old one */
    release(g_canvas);
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = { DXGI_FORMAT_R8G8B8A8_UNORM, { 0, 0, 0, 0 } };
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&g_canvas)))) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += g_rtv_size;
    g_device->CreateRenderTargetView(g_canvas, nullptr, rtv);
    g_device->CreateShaderResourceView(g_canvas, nullptr, g_srv_heap->GetCPUDescriptorHandleForHeapStart());
    g_canvas_w = w;
    g_canvas_h = h;
    return true;
}

static bool ensure_pso(DXGI_FORMAT format)
{
    if (g_pso && g_pso_format == format) return true;
    wait_for(g_fence_value);
    release(g_pso);
    ID3DBlob *vs = nullptr, *ps = nullptr, *error = nullptr;
    if (FAILED(D3DCompile(SHADER, strlen(SHADER), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &error)) ||
        FAILED(D3DCompile(SHADER, strlen(SHADER), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &error))) {
        logf("shader: %s", error ? (const char *)error->GetBufferPointer() : "compile failed");
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = g_root;
    desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE; /* premultiplied */
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    HRESULT hr = g_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_pso));
    vs->Release();
    ps->Release();
    g_pso_format = format;
    return SUCCEEDED(hr);
}

static void transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    g_list->ResourceBarrier(1, &b);
}

static const char *format_name(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "FP16";
    default: return "other";
    }
}

/* Draws the menu, or the text shown without it, onto the back buffer that is about to be presented. false disables the menu for good. */
static bool render(IDXGISwapChain *sc, const DXGI_SWAP_CHAIN_DESC &desc, float presents_per_second, const std::shared_ptr<const Menu> &menu)
{
    if (!g_ready) {
        if (!create_device_objects(sc)) return false;
        g_ready = true;
        logf("menu ready");
    }
    UINT w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
    if (!ensure_canvas(w, h) || !ensure_pso(desc.BufferDesc.Format)) return false;

    /* A float back buffer is always scRGB. A 10-bit one is HDR10 only when the game says HDR is on. */
    int mode = desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : (desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && menu->hdr) ? 2 : 0;
    float constants[2] = { (float)mode, mode == 1 ? menu->nits / 80.0f : mode == 2 ? menu->nits / 10000.0f : 1.0f };
    snprintf(g_status, sizeof g_status, "%ux%u %s %s, %.0f frames shown per second", w, h, format_name(desc.BufferDesc.Format),
             mode == 1 ? "scRGB" : mode == 2 ? "HDR10" : "SDR", presents_per_second);

    {
        std::lock_guard<std::recursive_mutex> hold(g_imgui_lock);
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        /* ponytail: the mouse is in window coordinates, so this assumes the back buffer is the
           size of the window's client area, which holds for borderless and full screen. Scale
           the mouse events here if the game ever renders the swap chain at another size. */
        ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
        ImGui::GetIO().MouseDrawCursor = g_open; /* the game hides its own pointer in flight */
        ImGui::NewFrame();
        if (g_open) draw_menu(*menu, (float)w, (float)h);
        else if (!menu->toast.empty()) draw_toast(*menu, (float)w, (float)h);
        if (!menu->stats.empty()) draw_stats(*menu, (float)w, (float)h);
        ImGui::Render();
    }

    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;
    UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource *back = nullptr;
    if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&back)))) return false;

    FrameContext &frame = g_frames[g_frame++ % 3];
    wait_for(frame.fence_value);
    frame.allocator->Reset();
    g_list->Reset(frame.allocator, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE back_rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart(), canvas_rtv = back_rtv;
    canvas_rtv.ptr += g_rtv_size;
    const float transparent[4] = { 0, 0, 0, 0 };
    transition(g_canvas, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_list->OMSetRenderTargets(1, &canvas_rtv, FALSE, nullptr);
    g_list->ClearRenderTargetView(canvas_rtv, transparent, 0, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srv_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
    transition(g_canvas, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_device->CreateRenderTargetView(back, nullptr, back_rtv);
    g_list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT scissor = { 0, 0, (LONG)w, (LONG)h };
    g_list->RSSetViewports(1, &viewport);
    g_list->RSSetScissorRects(1, &scissor);
    g_list->SetGraphicsRootSignature(g_root);
    g_list->SetPipelineState(g_pso);
    g_list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    g_list->SetGraphicsRootDescriptorTable(1, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
    transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    bool ok = SUCCEEDED(g_list->Close());
    if (ok) {
        ID3D12CommandList *lists[] = { g_list };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_fence, ++g_fence_value);
        frame.fence_value = g_fence_value;
    }
    back->Release(); /* the swap chain keeps it alive; holding it would block the game's resize */
    return ok;
}

static void on_present(IDXGISwapChain *sc)
{
    static LARGE_INTEGER freq, start;
    static int presents;
    static double window_start, last_seen, last_note;
    static float presents_per_second;
    static bool heartbeat;
    DXGI_SWAP_CHAIN_DESC desc;
    if (g_dead || FAILED(sc->GetDesc(&desc))) return;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double now = (double)(t.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    if (desc.OutputWindow != g_hwnd) {
        if (!g_test && !IsWindowVisible(desc.OutputWindow)) return; /* somebody's helper swap chain */
        if (g_hwnd && now - last_seen < 1.0) return;                 /* ours is still presenting */
        g_hwnd = desc.OutputWindow; /* first window, or the game moved to a new one */
        logf("the game presents %ux%u %s, %u buffers, window %p", desc.BufferDesc.Width, desc.BufferDesc.Height,
             format_name(desc.BufferDesc.Format), desc.BufferCount, (void *)g_hwnd);
    }
    g_now = last_seen = now;
    presents++;
    std::shared_ptr<const Menu> menu;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        menu = g_menu;
    }
    int toggle = menu->key & 255;
    if (g_now - window_start >= 1.0) {
        presents_per_second = (float)(presents / (g_now - window_start));
        presents = 0;
        window_start = g_now;
        /* Once a second: what the menu can see right now, for working out why a key does nothing.
           Only the text is made here; no file is touched on the thread that presents frames. */
        HWND front = GetForegroundWindow();
        DWORD front_pid = 0;
        GetWindowThreadProcessId(front, &front_pid);
        char text[512];
        snprintf(text, sizeof text, "seconds=%.0f\nframes_per_second=%.0f\nthread=%lu\nwindow=%p\nfront_window=%p\nfront_pid=%lu\nour_pid=%lu\n"
                 "game_in_front=%d\nmenu_key=%d\nmenu_key_down=%d\nopen=%d\nready=%d\n",
                 g_now, presents_per_second, GetCurrentThreadId(), (void *)g_hwnd, (void *)front, front_pid, GetCurrentProcessId(),
                 game_in_front() ? 1 : 0, toggle, (GetAsyncKeyState(toggle) & 0x8000) ? 1 : 0, g_open ? 1 : 0, g_ready ? 1 : 0);
        std::lock_guard<std::mutex> hold(g_lock);
        g_status_text = text;
        g_status_dirty = true;
    }
    if (!heartbeat && g_now > 10.0) {
        heartbeat = true;
        logf("still presenting after ten seconds, %.0f frames per second", presents_per_second);
    }
    if (!g_test && (GetAsyncKeyState(toggle) & 0x8000) && !game_in_front() && g_now - last_note > 5.0) {
        last_note = g_now;
        logf("the menu key is down but another program has the keyboard");
    }
    static bool was_open;
    int pressed = key_pressed(*menu);
    if (g_capturing) pressed = -2; /* that key is being picked, not used */
    if (pressed == -1) g_open = !g_open;
    else if (pressed >= 0 && !g_open) push_event("key\t%s", menu->hotkeys[pressed].id.c_str());
    if (g_open != was_open) { /* the menu key, or the X in the title bar on the frame before */
        was_open = g_open;
        g_capture.clear();
        g_capturing = false;
        push_event("menu\t%d", g_open ? 1 : 0);
        logf("menu %s", g_open ? "opened" : "closed");
        if (g_ready) { /* nothing held or half-typed carries over to the next time */
            std::lock_guard<std::recursive_mutex> hold(g_imgui_lock);
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
    }
    if (!g_open && menu->toast.empty() && menu->stats.empty()) return;
    if (!render(sc, desc, presents_per_second, menu)) {
        g_dead = true;
        logf("drawing failed, the menu is off until the game restarts");
    }
}

static HRESULT(STDMETHODCALLTYPE *o_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *o_present1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static thread_local bool t_inside;

/* An access violation in here must not take the game down with it. */
static void guarded_present(IDXGISwapChain *sc)
{
    __try {
        on_present(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        logf("crashed while drawing, the menu is off until the game restarts");
    }
}

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    if (!t_inside && !(flags & DXGI_PRESENT_TEST)) {
        t_inside = true;
        guarded_present(sc);
        t_inside = false;
    }
    return o_present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *params)
{
    if (!t_inside && !(flags & DXGI_PRESENT_TEST)) {
        t_inside = true;
        guarded_present(sc);
        t_inside = false;
    }
    return o_present1(sc, sync, flags, params);
}

static bool patch(void **vtable, int index, void *hook, void **original)
{
    DWORD old;
    if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_READWRITE, &old)) return false;
    *original = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void *), old, &old);
    return true;
}

/* Every swap chain dxgi.dll makes for a window shares one vtable, so a throwaway one gives us the game's. */
static bool install_hooks()
{
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AC8TweaksProbe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    IDXGIFactory2 *factory = nullptr;
    IDXGISwapChain1 *sc = nullptr;
    bool ok = false;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 64;
    sd.Height = 64;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (hwnd && SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
        SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc))) {
        g_queue_vtable = *(void **)queue;
        MEMORY_BASIC_INFORMATION mbi;
        for (size_t offset = 0; offset < 0x1000 && g_queue_offsets.size() < 4; offset += sizeof(void *)) {
            void **slot = (void **)((char *)sc + offset);
            if (!VirtualQuery(slot, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
            if (*slot == (void *)queue) g_queue_offsets.push_back(offset);
        }
        void **vtable = *(void ***)sc;
        ok = !g_queue_offsets.empty() && patch(vtable, 8, (void *)hk_present, (void **)&o_present) && patch(vtable, 22, (void *)hk_present1, (void **)&o_present1);
    }
    release(sc);
    release(factory);
    release(queue);
    release(device);
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

static DWORD WINAPI start(void *)
{
    if (!install_hooks()) {
        logf("could not reach the swap chain, no menu this session");
        return 0;
    }
    logf("hooks in place");
    worker_loop();
}

/* Test hook for the harness: stands in for the keyboard when AC8OVERLAY_TEST=1. */
extern "C" __declspec(dllexport) void ac8overlay_test_key(int vk, int down)
{
    g_test_keys[vk & 255] = down != 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, void *)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    wchar_t buf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"AC8TWEAKS_OFFLINE", buf, MAX_PATH) || wcscmp(buf, L"1") != 0) return TRUE; /* inert */
    if (!GetEnvironmentVariableW(L"AC8TWEAKS_ROOT", buf, MAX_PATH)) return TRUE;
    g_dir = std::wstring(buf) + L"\\AC8Tweaks\\";
    g_test = GetEnvironmentVariableW(L"AC8OVERLAY_TEST", buf, MAX_PATH) && wcscmp(buf, L"1") == 0;
    /* Lua frees the library when its state closes; the vtable must never point at unloaded code. */
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)self, &pinned);
    HANDLE thread = CreateThread(nullptr, 0, start, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    return TRUE;
}
