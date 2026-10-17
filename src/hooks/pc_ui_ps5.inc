// [ps5 port] The in-game menu on the console. Included by pc_ui.cpp in place of the desktop pages.
//
// A console settings screen rather than a desktop dialog: four pages switched with L1 and R1, one setting
// per row, up and down to move, left and right to change a value, Cross to switch or act, Circle to close.
// The row in focus explains itself in one line at the bottom. Black, white and the logo's red, with
// condensed type (Oswald for text, Bebas Neue for headings) like the game's own lettering.
//
// Needs from pc_ui.cpp: find(), commit(), restart_needed(), the pad capture (g_capture, capture_start,
// capture_cancel, capture_poll_pad, ui_capturing), ui_ticks(), g_font_display and g_font_body_mid.

namespace ps5ui {

const ImU32 kAccent    = IM_COL32(226, 30, 38, 255);
const ImU32 kText      = IM_COL32(240, 240, 240, 255);
const ImU32 kTextSoft  = IM_COL32(200, 200, 206, 255);
const ImU32 kTextDim   = IM_COL32(140, 140, 148, 255);
const ImU32 kTextFaint = IM_COL32(92, 92, 100, 255);
const ImU32 kRowFocus  = IM_COL32(255, 255, 255, 22);
const ImU32 kLine      = IM_COL32(255, 255, 255, 30);
const ImU32 kAmber     = IM_COL32(255, 186, 64, 255);

enum Page { VIDEO, CONTROLS, BUTTONS, GAME, PAGE_COUNT };
const char *const k_page_names[PAGE_COUNT] = { "VIDEO", "CONTROLS", "BUTTONS", "GAME" };

int g_page;
bool g_focus_first = true;      // put the cursor on the page's first row
std::string g_help;             // the focused row's line, shown at the bottom
UiTick g_reset_until;           // "reset all settings" waits this long for the second press

// ---- drawing --------------------------------------------------------------------------------------------------

ImFont *display_font() { return g_font_display ? g_font_display : ImGui::GetFont(); }

float font_mid(ImFont *f) { return f == g_font_display ? 0.458f : g_font_body_mid; }

ImVec2 text_size(ImFont *f, float size, const char *t) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, t); }

// Text whose capitals are centred on `cy`.
void text_mid(ImDrawList *dl, ImFont *f, float size, float x, float cy, ImU32 col, const char *t)
{
    dl->AddText(f, size, ImVec2(floorf(x + 0.5f), floorf(cy - size * font_mid(f) + 0.5f)), col, t);
}

// A PlayStation button as the system draws it: a white disc with the symbol in it, or a tab for L1 and R1.
enum Glyph { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_L1, G_R1 };

float draw_glyph(ImDrawList *dl, ImVec2 p, float h, Glyph g)
{
    const ImU32 ink = IM_COL32(18, 18, 20, 255), paper = IM_COL32(236, 236, 236, 255);
    if (g >= G_L1) {
        const char *name = g == G_L1 ? "L1" : "R1";
        ImFont *f = display_font();
        float fs = h * 0.95f, w = text_size(f, fs, name).x + h * 0.62f;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), paper, h * 0.28f);
        text_mid(dl, f, fs, p.x + h * 0.31f, p.y + h * 0.5f, ink, name);
        return w;
    }
    float r = h * 0.5f, k = r * 0.44f, t = h * 0.1f;
    ImVec2 c(p.x + r, p.y + r);
    dl->AddCircleFilled(c, r, paper, 32);
    switch (g) {
    case G_CROSS:
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), ink, t);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), ink, t);
        break;
    case G_CIRCLE:
        dl->AddCircle(c, k * 1.05f, ink, 24, t);
        break;
    case G_SQUARE:
        dl->AddRect(ImVec2(c.x - k * 0.9f, c.y - k * 0.9f), ImVec2(c.x + k * 0.9f, c.y + k * 0.9f), ink, 0.0f, 0, t);
        break;
    default:
        dl->AddTriangle(ImVec2(c.x, c.y - k * 1.05f), ImVec2(c.x + k * 1.1f, c.y + k * 0.8f),
                        ImVec2(c.x - k * 1.1f, c.y + k * 0.8f), ink, t);
        break;
    }
    return h;
}

void arrow(ImDrawList *dl, float x, float cy, float a, int dir, ImU32 col)
{
    if (dir < 0)
        dl->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + a, cy - a * 1.15f), ImVec2(x + a, cy + a * 1.15f), col);
    else
        dl->AddTriangleFilled(ImVec2(x, cy - a * 1.15f), ImVec2(x + a, cy), ImVec2(x, cy + a * 1.15f), col);
}

// ---- rows -----------------------------------------------------------------------------------------------------

enum RowKind { ROW_TOGGLE, ROW_CHOICE, ROW_SLIDER, ROW_ACTION, ROW_INFO };

struct Row {
    const char *label;
    const char *value;          // CHOICE, SLIDER, ACTION, INFO
    const char *help;
    RowKind kind;
    bool on;                    // TOGGLE
    float fraction;             // SLIDER, 0..1
    bool amber;                 // the value waits for something
};

enum { ROW_LEFT = -1, ROW_NONE = 0, ROW_RIGHT = 1, ROW_PRESS = 2 };

// One setting across the page: its name on the left, its value on the right. Returns what was asked of it.
int row(const char *id, const Row &r)
{
    float F = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = F * 1.75f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool pressed = false, focused = false;
    if (r.kind == ROW_INFO) {
        ImGui::Dummy(ImVec2(w, h * 0.86f));
        h *= 0.86f;
    } else {
        if (g_focus_first) {
            ImGui::SetKeyboardFocusHere();
            g_focus_first = false;
        }
        ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
        pressed = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        focused = ImGui::IsItemFocused();
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *f = ImGui::GetFont();
    float pad = F * 0.85f, cy = p.y + h * 0.5f;
    if (focused) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kRowFocus, F * 0.22f);
        dl->AddRectFilled(p, ImVec2(p.x + F * 0.16f, p.y + h), kAccent, F * 0.08f, ImDrawFlags_RoundCornersLeft);
    }
    text_mid(dl, f, F, p.x + pad, cy, focused ? kText : r.kind == ROW_INFO ? kTextDim : kTextSoft, r.label);

    float x1 = p.x + w - pad, vw = F * 9.5f, x0 = x1 - vw;
    ImU32 vcol = r.amber ? kAmber : focused || r.kind == ROW_INFO ? kText : kTextDim;
    switch (r.kind) {
    case ROW_TOGGLE: {
        float pw = F * 1.9f, ph = F * 1.0f, px = x1 - pw, py = cy - ph * 0.5f;
        dl->AddRectFilled(ImVec2(px, py), ImVec2(x1, py + ph), r.on ? kAccent : IM_COL32(255, 255, 255, 38), ph * 0.5f);
        float kr = ph * 0.5f - F * 0.12f;
        dl->AddCircleFilled(ImVec2(r.on ? x1 - ph * 0.5f : px + ph * 0.5f, cy), kr,
                            r.on ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 170, 176, 255), 24);
        const char *t = r.on ? "On" : "Off";
        text_mid(dl, f, F, px - F * 0.55f - text_size(f, F, t).x, cy, vcol, t);
        break;
    }
    case ROW_CHOICE: {
        float a = F * 0.26f;
        ImU32 ac = focused ? kAccent : kTextFaint;
        arrow(dl, x0, cy, a, -1, ac);
        arrow(dl, x1 - a, cy, a, 1, ac);
        float tw = text_size(f, F, r.value).x;
        text_mid(dl, f, F, (x0 + x1 - tw) * 0.5f, cy, vcol, r.value);
        break;
    }
    case ROW_SLIDER: {
        float tx1 = x1 - F * 3.0f, th = F * 0.14f;
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(tx1, cy + th * 0.5f), IM_COL32(255, 255, 255, 38), th);
        float kx = x0 + (tx1 - x0) * r.fraction;
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), focused ? kAccent : kTextDim, th);
        dl->AddCircleFilled(ImVec2(kx, cy), F * 0.3f, focused ? kText : kTextSoft, 20);
        text_mid(dl, f, F, x1 - text_size(f, F, r.value).x, cy, vcol, r.value);
        break;
    }
    case ROW_ACTION: {
        float a = F * 0.24f;
        arrow(dl, x1 - a, cy, a, 1, focused ? kAccent : kTextFaint);
        if (r.value && *r.value)
            text_mid(dl, f, F, x1 - a - F * 0.6f - text_size(f, F, r.value).x, cy, vcol, r.value);
        break;
    }
    case ROW_INFO:
        if (r.value && *r.value)
            text_mid(dl, f, F, x1 - text_size(f, F, r.value).x, cy, kTextSoft, r.value);
        break;
    }

    int asked = ROW_NONE;
    if (focused) {
        if (r.help) g_help = r.help;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft))
            asked = ROW_LEFT;
        else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight))
            asked = ROW_RIGHT;
    }
    if (pressed) asked = ROW_PRESS;
    ImGui::PopID();
    return asked;
}

void section(const char *title)
{
    float F = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, F * 0.5f));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float fs = F * 0.95f, h = F * 1.15f, cy = p.y + h * 0.5f;
    float tw = text_size(display_font(), fs, title).x;
    text_mid(dl, display_font(), fs, p.x + F * 0.85f, cy, kAccent, title);
    dl->AddLine(ImVec2(p.x + F * 1.4f + tw, cy), ImVec2(p.x + w - F * 0.85f, cy), kLine, 1.0f);
    ImGui::Dummy(ImVec2(w, h));
}

void set_value(RecompSetting *s, int v)
{
    if (s->type == RECOMP_SETTING_INT) v = v < s->min ? s->min : v > s->max ? s->max : v;
    if (v == s->stored) return;
    recomp_settings_set(s->section, s->key, v);
    commit();
}

void toggle_row(const char *section_name, const char *key, const char *label, const char *help)
{
    RecompSetting *s = find(section_name, key);
    if (!s) return;
    Row r = { label, nullptr, help, ROW_TOGGLE, s->stored != 0, 0.0f, false };
    if (row(key, r) != ROW_NONE) set_value(s, s->stored ? 0 : 1);
}

// A choice among `n` of the setting's values, shown with `names`; left and right go round.
void choice_row(const char *section_name, const char *key, const char *label, const char *help,
                const char *const *names, const int *values, int n)
{
    RecompSetting *s = find(section_name, key);
    if (!s) return;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (values[i] == s->stored) cur = i;
    const char *shown = cur >= 0 ? names[cur] : s->choices ? s->choices[s->stored] : "?";
    Row r = { label, shown, help, ROW_CHOICE, false, 0.0f, false };
    int asked = row(key, r);
    if (asked == ROW_NONE) return;
    int step = asked == ROW_LEFT ? -1 : 1;
    set_value(s, values[cur < 0 ? 0 : (cur + step + n) % n]);
}

void slider_row(const char *section_name, const char *key, const char *label, const char *help, int step,
                const char *zero_text = nullptr)
{
    RecompSetting *s = find(section_name, key);
    if (!s) return;
    char value[32];
    if (s->stored == 0 && zero_text) snprintf(value, sizeof value, "%s", zero_text);
    else snprintf(value, sizeof value, "%d%%", s->stored);
    float span = (float)(s->max - s->min);
    Row r = { label, value, help, ROW_SLIDER, false, span > 0 ? (s->stored - s->min) / span : 0.0f, false };
    int asked = row(key, r);
    if (asked == ROW_LEFT) set_value(s, (s->stored - step) / step * step);
    else if (asked == ROW_RIGHT) set_value(s, (s->stored + step) / step * step);
}

void info_row(const char *id, const char *label, const char *value)
{
    Row r = { label, value, nullptr, ROW_INFO, false, 0.0f, false };
    row(id, r);
}

// ---- the pages --------------------------------------------------------------------------------------------------

void page_video()
{
    section("PICTURE");
    {
        static const char *const names[] = { "4:3", "Stretch", "16:9" };
        static const int values[] = { 0, 1, 3 };
        choice_row("display", "aspect", "Screen shape",
                   "16:9 widens the fights. Menus and movies keep their 4:3 shape.", names, values, 3);
    }
    if (recomp_settings_get("display", "aspect", 0) >= 2) {
        // The side fill and, for the border, which picture: one list.
        static const char *const fill[] = { "Logo border", "Red border", "Gold border", "Plain border", "Blur", "Black" };
        RecompSetting *mode = find("display", "widescreen_side_fill"), *pick = find("display", "border");
        if (mode && pick) {
            int cur = mode->stored == 2 ? (pick->stored >= 0 && pick->stored < 4 ? pick->stored : 0) : mode->stored == 0 ? 4 : 5;
            Row r = { "Beside 4:3 screens", fill[cur], "What fills the sides of menus and movies. The borders are in borders/.",
                      ROW_CHOICE, false, 0.0f, false };
            int asked = row("side_fill", r);
            if (asked != ROW_NONE) {
                cur = (cur + (asked == ROW_LEFT ? -1 : 1) + 6) % 6;
                if (cur < 4) {
                    set_value(mode, 2);
                    set_value(pick, cur);
                } else {
                    set_value(mode, cur == 4 ? 0 : 1);
                }
            }
        }
        static const char *const scenes[] = { "As made", "Fill" };
        static const int scene_values[] = { 0, 1 };
        choice_row("display", "widescreen_cutscenes", "Story scenes",
                   "Framed as the game made them, or filling the screen.", scenes, scene_values, 2);
    }
    {
        static const char *const names[] = { "Smooth", "Sharp" };
        static const int values[] = { 0, 1 };
        choice_row("display", "filter", "Picture filter", "How the picture is scaled to the TV.", names, values, 2);
    }

    section("QUALITY");
    if (RecompSetting *s = find("display", "render_scale")) {
        bool wide = recomp_settings_get("display", "aspect", 0) >= 2;
        char value[48];
        int across = wide ? (int)(480.0f * s->stored * 16.0f / 9.0f + 0.5f) : 640 * s->stored;
        snprintf(value, sizeof value, "%dx  ·  %d×%d", s->stored, across, 480 * s->stored);
        Row r = { "Resolution", value, "How sharp the game is drawn. 4x is close to 4K; lower it if fights slow down.",
                  ROW_CHOICE, false, 0.0f, false };
        int asked = row("render_scale", r);
        if (asked == ROW_LEFT) set_value(s, s->stored > s->min ? s->stored - 1 : s->max);
        else if (asked != ROW_NONE) set_value(s, s->stored < s->max ? s->stored + 1 : s->min);
    }
    toggle_row("display", "output_4k", "4K output", "Send 3840×2160 to the TV. Off sends 1080p.");
    toggle_row("textures", "enabled", "HD textures", "The remastered menu and HUD pictures in the mods folder.");
    toggle_row("display", "sharp_text", "Sharper text", "Menu text redrawn with clean edges.");
    toggle_row("display", "gamma", "Original brightness", "The game's own brightness curve, as it shipped.");

    section("SCREEN");
    toggle_row("ui", "show_fps", "Frame counter", "Frames per second in the corner.");
    toggle_row("display", "frame_gen", "Frame generation (beta)",
               "120 fps on a 120 Hz TV: a made-up picture between every two frames. Adds a little input lag.");
}

const char *pad_source_label(int src)
{
    static const struct { int src; const char *name; } names[] = {
        { INPUT_SRC_SOUTH, "Cross" }, { INPUT_SRC_EAST, "Circle" }, { INPUT_SRC_WEST, "Square" },
        { INPUT_SRC_NORTH, "Triangle" }, { INPUT_SRC_LEFT_SHOULDER, "L1" }, { INPUT_SRC_RIGHT_SHOULDER, "R1" },
        { INPUT_SRC_LEFT_TRIGGER, "L2" }, { INPUT_SRC_RIGHT_TRIGGER, "R2" },
        { INPUT_SRC_LEFT_STICK_CLICK, "L3" }, { INPUT_SRC_RIGHT_STICK_CLICK, "R3" },
        { INPUT_SRC_START, "Options" }, { INPUT_SRC_BACK, "Touchpad" },
        { INPUT_SRC_DPAD_UP, "Up" }, { INPUT_SRC_DPAD_DOWN, "Down" }, { INPUT_SRC_DPAD_LEFT, "Left" },
        { INPUT_SRC_DPAD_RIGHT, "Right" },
        { INPUT_SRC_LSTICK_UP, "Left stick up" }, { INPUT_SRC_LSTICK_DOWN, "Left stick down" },
        { INPUT_SRC_LSTICK_LEFT, "Left stick left" }, { INPUT_SRC_LSTICK_RIGHT, "Left stick right" },
        { INPUT_SRC_RSTICK_UP, "Right stick up" }, { INPUT_SRC_RSTICK_DOWN, "Right stick down" },
        { INPUT_SRC_RSTICK_LEFT, "Right stick left" }, { INPUT_SRC_RSTICK_RIGHT, "Right stick right" },
        { INPUT_SRC_NONE, "Nothing" },
    };
    for (const auto &n : names)
        if (n.src == src) return n.name;
    const char *raw = input_source_name(src);
    return raw ? raw : "?";
}

// The order left and right go through.
const int k_pad_sources[] = {
    INPUT_SRC_SOUTH, INPUT_SRC_EAST, INPUT_SRC_WEST, INPUT_SRC_NORTH,
    INPUT_SRC_LEFT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER, INPUT_SRC_LEFT_TRIGGER, INPUT_SRC_RIGHT_TRIGGER,
    INPUT_SRC_LEFT_STICK_CLICK, INPUT_SRC_RIGHT_STICK_CLICK, INPUT_SRC_START, INPUT_SRC_BACK,
    INPUT_SRC_DPAD_UP, INPUT_SRC_DPAD_DOWN, INPUT_SRC_DPAD_LEFT, INPUT_SRC_DPAD_RIGHT,
    INPUT_SRC_LSTICK_UP, INPUT_SRC_LSTICK_DOWN, INPUT_SRC_LSTICK_LEFT, INPUT_SRC_LSTICK_RIGHT,
    INPUT_SRC_RSTICK_UP, INPUT_SRC_RSTICK_DOWN, INPUT_SRC_RSTICK_LEFT, INPUT_SRC_RSTICK_RIGHT,
    INPUT_SRC_NONE,
};

void draw_tester()
{
    float F = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = F * 3.6f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *f = ImGui::GetFont();
    InputRaw raw;
    int slot = -1;
    for (int i = 0; i < 4 && slot < 0; i++)
        if (xbox_HostInputRawPad(i, &raw)) slot = i;
    float cy = p.y + h * 0.5f, x = p.x + F * 0.85f;
    if (slot < 0) {
        text_mid(dl, f, F, x, cy, kTextDim, "Connect a controller to test it.");
        return;
    }
    InputPadMap map;
    pc_input_current_padmap(&map);
    InputPad pad;
    input_pad_map(&map, &raw, &pad);
    float r = F * 1.35f;
    for (int stick = 0; stick < 2; stick++) {
        ImVec2 c(x + r, cy);
        float sx = (stick ? pad.rx : pad.lx) / 32767.0f, sy = (stick ? pad.ry : pad.ly) / 32767.0f;
        dl->AddCircle(c, r, IM_COL32(255, 255, 255, 60), 40, F * 0.08f);
        dl->AddLine(ImVec2(c.x - r * 0.25f, c.y), ImVec2(c.x + r * 0.25f, c.y), kLine);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.25f), ImVec2(c.x, c.y + r * 0.25f), kLine);
        dl->AddCircleFilled(ImVec2(c.x + sx * r * 0.85f, c.y - sy * r * 0.85f), F * 0.26f, kAccent, 20);
        x += r * 2 + F * 0.8f;
    }
    static const char *const analog[8] = { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2" };
    static const char *const digital[8] = { "Up", "Down", "Left", "Right", "Options", "Touchpad", "L3", "R3" };
    std::string held;
    for (int i = 0; i < 8; i++)
        if (pad.analog[i] > 30) { held += held.empty() ? "" : "  "; held += analog[i]; }
    for (int i = 0; i < 8; i++)
        if (pad.buttons & (1 << i)) { held += held.empty() ? "" : "  "; held += digital[i]; }
    x += F * 0.4f;
    text_mid(dl, display_font(), F * 0.85f, x, cy - F * 0.62f, kTextDim, "THE GAME SEES");
    text_mid(dl, f, F, x, cy + F * 0.5f, held.empty() ? kTextFaint : kText, held.empty() ? "Nothing pressed" : held.c_str());
}

void page_controls()
{
    section("CONTROLLERS");
    int shown = 0;
    for (int slot = 0; slot < 4; slot++) {
        char name[64], label[16], id[16];
        int what = xbox_HostInputSlotInfo(slot, name, sizeof name);
        if (!what) continue;
        snprintf(label, sizeof label, "Player %d", slot + 1);
        snprintf(id, sizeof id, "slot%d", slot);
        Row r = { label, name, "Press Cross to test the vibration.", (what & 1) ? ROW_ACTION : ROW_INFO, false, 0.0f, false };
        if (row(id, r) == ROW_PRESS) xbox_HostInputRumbleTest(slot);
        shown++;
    }
    if (!shown) info_row("none", "No controller connected", "");

    section("VIBRATION");
    slider_row("input", "rumble", "Strength", "How hard the controller vibrates.", 10, "Off");
    toggle_row("input", "rumble_on_connect", "Buzz on connect", "A short buzz when a controller connects.");

    section("STICKS AND TRIGGERS");
    slider_row("gamepad", "left_deadzone", "Left stick dead zone",
               "Extra play before the stick counts. The game has its own.", 5, "None");
    slider_row("gamepad", "right_deadzone", "Right stick dead zone",
               "Extra play before the stick counts. The game has its own.", 5, "None");
    slider_row("gamepad", "trigger_threshold", "Trigger press point", "How far L2 and R2 go down before they count.", 5);

    section("TEST");
    draw_tester();
}

void page_buttons()
{
    static const struct { int control; const char *name; } controls[] = {
        { INPUT_A, "Cross" }, { INPUT_B, "Circle" }, { INPUT_X, "Square" }, { INPUT_Y, "Triangle" },
        { INPUT_BLACK, "L1" }, { INPUT_WHITE, "R1" }, { INPUT_LEFT_TRIGGER, "L2" }, { INPUT_RIGHT_TRIGGER, "R2" },
        { INPUT_LEFT_THUMB, "L3" }, { INPUT_RIGHT_THUMB, "R3" }, { INPUT_START, "Options" }, { INPUT_BACK, "Touchpad" },
        { INPUT_DPAD_UP, "Up" }, { INPUT_DPAD_DOWN, "Down" }, { INPUT_DPAD_LEFT, "Left" }, { INPUT_DPAD_RIGHT, "Right" },
    };
    const int n_sources = (int)(sizeof k_pad_sources / sizeof k_pad_sources[0]);
    bool detecting = g_capture.kind == Capture::PAD;
    if (detecting && ui_ticks() > g_capture.armed_at + 5000) {
        capture_cancel();                       // nothing pressed: leave it as it was
        detecting = false;
    }

    section("LAYOUT");
    {
        Row r = { "Reset to default", "", "Every button back to where the game had it.", ROW_ACTION, false, 0.0f, false };
        if (row("reset", r) == ROW_PRESS) {
            for (const auto &c : controls) recomp_settings_reset_to_default("gamepad", input_control_name(c.control));
            commit();
        }
    }

    section("IN THE GAME  ·  YOUR BUTTON");
    for (const auto &c : controls) {
        const char *key = input_control_name(c.control);
        RecompSetting *s = find("gamepad", key);
        if (!s) continue;
        int cur = input_source_from_name(s->stored_text);
        bool this_one = detecting && g_capture.key == key;
        Row r = { c.name, this_one ? "Press a button…" : pad_source_label(cur < 0 ? INPUT_SRC_NONE : cur),
                  this_one ? "Press the button to use. Nothing in 5 seconds keeps the old one."
                           : "Cross, then press a button to use it. Left and right pick from a list.",
                  ROW_CHOICE, false, 0.0f, this_one };
        int asked = row(key, r);
        if (asked == ROW_PRESS) {
            capture_start(Capture::PAD, "gamepad", key, false);
        } else if (asked != ROW_NONE) {
            int at = 0;
            for (int i = 0; i < n_sources; i++)
                if (k_pad_sources[i] == cur) at = i;
            at = (at + (asked == ROW_LEFT ? -1 : 1) + n_sources) % n_sources;
            recomp_settings_set_text("gamepad", key, input_source_name(k_pad_sources[at]));
            commit();
        }
    }
}

void page_game()
{
    section("MENU");
    slider_row("ui", "scale", "Menu size", "The size of this menu.", 10);
    info_row("open", "Open this menu", "Hold L3 + R3");

    section("PROFILE");
    toggle_row("debug", "unlock_all", "Unlock all fighters and venues",
               "For testing. It changes your profile and saves it, so keep it off on a save you care about.");

    section("SETTINGS");
    {
        bool armed = ui_ticks() < g_reset_until;
        Row r = { "Reset all settings", armed ? "Press Cross again" : "", "Every option on these pages back to default.",
                  ROW_ACTION, false, 0.0f, armed };
        if (row("reset_all", r) == ROW_PRESS) {
            if (!armed) {
                g_reset_until = ui_ticks() + 3000;
            } else {
                for (size_t i = 0; i < recomp_settings_count(); i++) {
                    RecompSetting *s = recomp_settings_at(i);
                    if (strcmp(s->section, "ps5"))
                        recomp_settings_reset_to_default(s->section, s->key);
                }
                commit();
                g_reset_until = 0;
            }
        }
    }

    section("ABOUT");
    info_row("version", "Version", DEFJAM_VERSION);
    info_row("port", "PlayStation 5 port", "@elripalda");
    info_row("built", "Built on", "DefJamFFNY-recomp  ·  xboxrecomp");
    info_row("licences", "Also uses", "Dear ImGui  ·  SDL3  ·  Bebas Neue  ·  Oswald");
}

// ---- the frame around them ------------------------------------------------------------------------------------

void header()
{
    float F = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *d = display_font();
    float big = F * 2.1f, h = F * 1.75f, cy = p.y + h * 0.5f;
    const char *title = "DEF JAM: FIGHT FOR NY";
    text_mid(dl, d, big, p.x, cy, kText, title);
    float x = p.x + text_size(d, big, title).x + F * 0.55f;
    float ts = F * 1.05f, tw = text_size(d, ts, "REMASTERED").x, th = F * 0.95f;
    dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + tw + F * 0.6f, cy + th * 0.5f), kAccent, F * 0.12f);
    text_mid(dl, d, ts, x + F * 0.3f, cy, kText, "REMASTERED");
    float ss = F * 1.3f;
    text_mid(dl, d, ss, p.x + w - text_size(d, ss, "SETTINGS").x, cy, kTextDim, "SETTINGS");
    ImGui::Dummy(ImVec2(w, h + F * 0.35f));
}

void tabs()
{
    float F = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *d = display_font();
    float h = F * 1.8f, fs = F * 1.3f, gh = F * 0.95f, cy = p.y + h * 0.5f;
    float x = p.x;
    x += draw_glyph(dl, ImVec2(x, cy - gh * 0.5f), gh, G_L1) + F * 1.1f;
    for (int i = 0; i < PAGE_COUNT; i++) {
        float tw = text_size(d, fs, k_page_names[i]).x;
        bool sel = i == g_page;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, h)) && g_page != i) {     // touch or mouse; the pad uses L1/R1
            g_page = i;
            g_focus_first = true;
        }
        ImGui::PopID();
        text_mid(dl, d, fs, x, cy, sel ? kText : kTextDim, k_page_names[i]);
        if (sel)
            dl->AddRectFilled(ImVec2(x, p.y + h - F * 0.16f), ImVec2(x + tw, p.y + h), kAccent);
        x += tw + F * 1.5f;
    }
    x += F * 0.1f - F * 0.4f;
    draw_glyph(dl, ImVec2(x, cy - gh * 0.5f), gh, G_R1);
    dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h), kLine, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + F * 0.2f));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

float footer_height() { return ImGui::GetFontSize() * 3.5f; }

void footer()
{
    float F = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *f = ImGui::GetFont(), *d = display_font();
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), kLine, 1.0f);
    if (!g_help.empty())
        dl->AddText(f, F * 0.95f, ImVec2(p.x + F * 0.85f, p.y + F * 0.35f), kTextSoft, g_help.c_str(), nullptr, w - F * 1.7f);

    // What the buttons do, from the right.
    float cy = p.y + F * 2.75f, gh = F * 0.9f, fs = F * 1.0f, x = p.x + w - F * 0.85f;
    struct Legend { Glyph g; const char *what; } legend[] = { { G_L1, "PAGES" }, { G_CIRCLE, "CLOSE" }, { G_CROSS, "SELECT" } };
    for (const Legend &l : legend) {
        x -= text_size(d, fs, l.what).x;
        text_mid(dl, d, fs, x, cy, kTextSoft, l.what);
        x -= F * 0.35f;
        if (l.g == G_L1) {
            float r1 = draw_glyph(dl, ImVec2(-1000, 0), gh, G_R1);   // its width
            x -= r1;
            draw_glyph(dl, ImVec2(x, cy - gh * 0.5f), gh, G_R1);
            x -= F * 0.2f + r1;
            draw_glyph(dl, ImVec2(x, cy - gh * 0.5f), gh, G_L1);
        } else {
            x -= gh;
            draw_glyph(dl, ImVec2(x, cy - gh * 0.5f), gh, l.g);
        }
        x -= F * 1.2f;
    }
    if (!restart_needed().empty())
        text_mid(dl, f, F * 0.95f, p.x + F * 0.85f, cy, kAmber, "Restart the game to apply your changes.");
    ImGui::Dummy(ImVec2(w, footer_height() - ImGui::GetStyle().ItemSpacing.y));
}

} // namespace ps5ui

UiResult ui_draw(UiMode, float x, float y, float width, float height)
{
    using namespace ps5ui;
    UiResult result;
    capture_poll_pad();
    if (!ui_capturing()) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) { g_page = (g_page + PAGE_COUNT - 1) % PAGE_COUNT; g_focus_first = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) { g_page = (g_page + 1) % PAGE_COUNT; g_focus_first = true; }
    }

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##settings", nullptr, flags);
    if (ImGui::IsWindowAppearing()) {
        g_focus_first = true;
        g_reset_until = 0;
    }
    {
        // The game's red along the top edge.
        ImVec2 wp = ImGui::GetWindowPos();
        float F = ImGui::GetFontSize(), rounding = ImGui::GetStyle().WindowRounding;
        ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + width, wp.y + F * 0.14f + rounding * 0.3f), kAccent,
                                                  rounding, ImDrawFlags_RoundCornersTop);
    }
    header();
    tabs();

    g_help.clear();
    if (g_focus_first) ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(k_page_names[g_page], ImVec2(0, -footer_height()), ImGuiChildFlags_NavFlattened);
    switch (g_page) {
    case VIDEO: page_video(); break;
    case CONTROLS: page_controls(); break;
    case BUTTONS: page_buttons(); break;
    default: page_game(); break;
    }
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
    ImGui::EndChild();
    footer();
    ImGui::End();
    return result;
}
