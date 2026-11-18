/* Physical controllers on HarmonyOS (GameControllerKit, API 21). The library is loaded at run time, so a system without it
   still starts the game, with the on-screen controls only.
   Every connected gamepad feeds one Switch controller, laid out as in the Android app (runtime/frontend_android.c): face
   buttons by label by default (the pad's A is the Switch's A); TOTK_FACE_BUTTONS=position maps them by position, for
   Xbox-layout pads. GameControllerKit has no monitor for the Select / View button, so the game page passes that key
   (KEYCODE_BUTTON_SELECT) here as Minus (totk_ohos_pad_key). Sticks get the Android app's 12 % dead zone.
   frontend_ohos.c merges this state with the on-screen controls in runtime_gamepad_read; the game page asks
   totk_ohos_gamepad_status to hide the on-screen controls while a controller is connected. */
#include "runtime.h"
#include <GameControllerKit/game_device.h>
#include <GameControllerKit/game_pad.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SW_A = 0, SW_B, SW_X, SW_Y, SW_LS, SW_RS, SW_L, SW_R, SW_ZL, SW_ZR, SW_PLUS, SW_MINUS, SW_LEFT, SW_UP, SW_RIGHT, SW_DOWN };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t keys, hat, trig, edges;   /* buttons from button events, from the D-pad axis, from trigger axes; presses since the last read */
static float axes[4];                     /* lx, ly, rx, ry in -1..1, up positive */
static int seen, devices, available = -1; /* available: -1 not tried, 0 no GameControllerKit, 1 monitors registered */
static char device_name[96];
static int by_label = 1;

typedef GameController_ErrorCode (*button_reg_fn)(GamePad_ButtonInputMonitorCallback);
typedef GameController_ErrorCode (*axis_reg_fn)(GamePad_AxisInputMonitorCallback);
typedef GameController_ErrorCode (*axis_get_fn)(const struct GamePad_AxisEvent *, double *);
static GameController_ErrorCode (*p_action)(const struct GamePad_ButtonEvent *, GamePad_Button_ActionType *);
static GameController_ErrorCode (*p_source)(const struct GamePad_AxisEvent *, GamePad_AxisSourceType *);
static axis_get_fn p_x, p_y, p_z, p_rz, p_hat_x, p_hat_y, p_brake, p_gas;
static GameController_ErrorCode (*p_dev_type)(const struct GameDevice_DeviceEvent *, GameDevice_StatusChangedType *);
static GameController_ErrorCode (*p_dev_info)(const struct GameDevice_DeviceEvent *, GameDevice_DeviceInfo **);
static GameController_ErrorCode (*p_info_name)(const struct GameDevice_DeviceInfo *, char **);
static GameController_ErrorCode (*p_info_destroy)(GameDevice_DeviceInfo **);
static GameController_ErrorCode (*p_info_type)(const struct GameDevice_DeviceInfo *, GameDevice_DeviceType *);

static void key_locked(int bit, int down) {
    if (down) { if (!(keys >> bit & 1)) edges |= 1ull << bit; keys |= 1ull << bit; }
    else keys &= ~(1ull << bit);
}
static int face(int bit) {   /* by position: the pad's bottom button (its A) is the Switch's bottom button (B), and so on */
    if (by_label) return bit;
    switch (bit) { case SW_A: return SW_B; case SW_B: return SW_A; case SW_X: return SW_Y; case SW_Y: return SW_X; }
    return bit;
}
static void button(const struct GamePad_ButtonEvent *e, int bit) {
    GamePad_Button_ActionType action = UP;
    if (!e || p_action(e, &action) != GAME_CONTROLLER_SUCCESS) return;
    pthread_mutex_lock(&mu);
    key_locked(bit, action == DOWN);
    seen = 1;
    pthread_mutex_unlock(&mu);
}
#define BUTTON(name, bit) static void on_##name(const struct GamePad_ButtonEvent *e) { button(e, bit); }
BUTTON(a, face(SW_A)) BUTTON(b, face(SW_B)) BUTTON(x, face(SW_X)) BUTTON(y, face(SW_Y))
BUTTON(l, SW_L) BUTTON(r, SW_R) BUTTON(zl, SW_ZL) BUTTON(zr, SW_ZR) BUTTON(plus, SW_PLUS)
BUTTON(left, SW_LEFT) BUTTON(up, SW_UP) BUTTON(right, SW_RIGHT) BUTTON(down, SW_DOWN) BUTTON(ls, SW_LS) BUTTON(rs, SW_RS)

static double get(axis_get_fn f, const struct GamePad_AxisEvent *e) {
    double v = 0;
    if (!f || f(e, &v) != GAME_CONTROLLER_SUCCESS || !isfinite(v)) return 0;
    return v;
}
/* The kit's value range is not documented: -1..1 (sticks) and 0..1 (triggers) are expected; raw 16-bit or 8-bit values
   are scaled down so a pad reporting those still works. The first event of each kind is logged with its raw values. */
static float unit(double v, double full) { if (fabs(v) > 1.0001) v /= full; return (float)(v > 1 ? 1 : v < -1 ? -1 : v); }
static float trigger_unit(double v) { return unit(fabs(v), fabs(v) <= 255.5 ? 255.0 : 1023.0); }
static void on_axis(const struct GamePad_AxisEvent *e) {
    GamePad_AxisSourceType source;
    if (!e || p_source(e, &source) != GAME_CONTROLLER_SUCCESS) return;
    double x = get(p_x, e), y = get(p_y, e), z = get(p_z, e), rz = get(p_rz, e), hx = get(p_hat_x, e), hy = get(p_hat_y, e);
    double brake = get(p_brake, e), gas = get(p_gas, e);
    static unsigned logged;
    if ((unsigned)source < 8 && !(logged >> source & 1)) {
        logged |= 1u << source;
        fprintf(stderr, "[totk-ohos] controller axis source %d first values: x %.3f y %.3f z %.3f rz %.3f hat %.3f %.3f brake %.3f gas %.3f\n",
                (int)source, x, y, z, rz, hx, hy, brake, gas);
    }
    pthread_mutex_lock(&mu);
    switch (source) {
        case LEFT_THUMBSTICK: axes[0] = unit(x, 32767); axes[1] = -unit(y, 32767); break;   /* the kit's down is +y, the Switch's up is + */
        case RIGHT_THUMBSTICK:
            if (z || rz) { x = z; y = rz; }   /* some pads report the right stick on Z / RZ */
            axes[2] = unit(x, 32767); axes[3] = -unit(y, 32767); break;
        case LEFT_TRIGGER: case RIGHT_TRIGGER: {
            double v = fmax(fmax(fabs(brake), fabs(gas)), fmax(fmax(fabs(z), fabs(rz)), fmax(fabs(x), fabs(y))));
            uint64_t bit = 1ull << (source == LEFT_TRIGGER ? SW_ZL : SW_ZR);
            uint64_t now = trigger_unit(v) > 0.5f ? bit : 0;
            if (now && !(trig & bit)) edges |= bit;
            trig = (trig & ~bit) | now;
            break;
        }
        case DPAD: {
            if (!hx && !hy) { hx = x; hy = y; }
            uint64_t h = (hx < -0.5 ? 1ull << SW_LEFT : 0) | (hx > 0.5 ? 1ull << SW_RIGHT : 0) |
                         (hy < -0.5 ? 1ull << SW_UP : 0) | (hy > 0.5 ? 1ull << SW_DOWN : 0);
            edges |= h & ~hat; hat = h;
            break;
        }
        default: break;
    }
    seen = 1;
    pthread_mutex_unlock(&mu);
}
static void clear_locked(void) { keys = hat = trig = edges = 0; memset(axes, 0, sizeof axes); }
static void on_device(const struct GameDevice_DeviceEvent *e) {
    GameDevice_StatusChangedType change = OFFLINE;
    if (!e || !p_dev_type || p_dev_type(e, &change) != GAME_CONTROLLER_SUCCESS) return;
    char name[sizeof device_name] = "";
    int type = -1;   /* GameDevice_DeviceType: 1 gamepad, 0 unknown; -1 not reported */
    GameDevice_DeviceInfo *info = NULL;
    if (p_dev_info && p_dev_info(e, &info) == GAME_CONTROLLER_SUCCESS && info) {
        char *n = NULL;   /* the kit's copy; not freed (its owner is not documented), a few bytes per connection */
        if (p_info_name && p_info_name(info, &n) == GAME_CONTROLLER_SUCCESS && n) snprintf(name, sizeof name, "%s", n);
        GameDevice_DeviceType dt;
        if (p_info_type && p_info_type(info, &dt) == GAME_CONTROLLER_SUCCESS) type = (int)dt;
        if (p_info_destroy) p_info_destroy(&info);
    }
    /* Only gamepads hide the on-screen controls: a Bluetooth watch (HUAWEI WATCH GT 6 Pro) is reported to the kit as a
       device of unknown type, and counting it took the touch controls away with no controller to play with. */
    if (type == 0) {
        fprintf(stderr, "[totk-ohos] input device %s: %s (not a gamepad: the on-screen controls stay)\n", change == ONLINE ? "connected" : "disconnected", name[0] ? name : "unnamed");
        return;
    }
    pthread_mutex_lock(&mu);
    if (change == ONLINE) { devices++; if (name[0]) memcpy(device_name, name, sizeof device_name); }
    else { if (devices > 0) devices--; if (!devices) { clear_locked(); device_name[0] = 0; } }
    int n = devices;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "[totk-ohos] controller %s: %s (type %d, %d connected)\n", change == ONLINE ? "connected" : "disconnected",
            name[0] ? name : "unnamed", type, n);
}

/* Once, from totk_ohos_start: load GameControllerKit and register every monitor. */
__attribute__((visibility("hidden"))) void ohos_gamepad_start(void) {
    pthread_mutex_lock(&mu);
    if (available >= 0) { pthread_mutex_unlock(&mu); return; }
    available = 0;
    pthread_mutex_unlock(&mu);
    const char *f = getenv("TOTK_FACE_BUTTONS"); by_label = !(f && !strcmp(f, "position"));
    void *lib = dlopen("libohgame_controller.z.so", RTLD_NOW);
    if (!lib) { fprintf(stderr, "[totk-ohos] controllers: GameControllerKit not available (%s); on-screen controls only\n", dlerror()); return; }
#define SYM(var, name) (*(void **)&var = dlsym(lib, name))
    SYM(p_action, "OH_GamePad_ButtonEvent_GetButtonAction"); SYM(p_source, "OH_GamePad_AxisEvent_GetAxisSourceType");
    SYM(p_x, "OH_GamePad_AxisEvent_GetXAxisValue"); SYM(p_y, "OH_GamePad_AxisEvent_GetYAxisValue");
    SYM(p_z, "OH_GamePad_AxisEvent_GetZAxisValue"); SYM(p_rz, "OH_GamePad_AxisEvent_GetRZAxisValue");
    SYM(p_hat_x, "OH_GamePad_AxisEvent_GetHatXAxisValue"); SYM(p_hat_y, "OH_GamePad_AxisEvent_GetHatYAxisValue");
    SYM(p_brake, "OH_GamePad_AxisEvent_GetBrakeAxisValue"); SYM(p_gas, "OH_GamePad_AxisEvent_GetGasAxisValue");
    SYM(p_dev_type, "OH_GameDevice_DeviceEvent_GetChangedType"); SYM(p_dev_info, "OH_GameDevice_DeviceEvent_GetDeviceInfo");
    SYM(p_info_name, "OH_GameDevice_DeviceInfo_GetName"); SYM(p_info_destroy, "OH_GameDevice_DestroyDeviceInfo");
    SYM(p_info_type, "OH_GameDevice_DeviceInfo_GetDeviceType");
    if (!p_action || !p_source) { fprintf(stderr, "[totk-ohos] controllers: GameControllerKit lacks its event accessors; on-screen controls only\n"); return; }
    static const struct { const char *name; GamePad_ButtonInputMonitorCallback cb; } buttons[] = {
        {"OH_GamePad_ButtonA_RegisterButtonInputMonitor", on_a}, {"OH_GamePad_ButtonB_RegisterButtonInputMonitor", on_b},
        {"OH_GamePad_ButtonX_RegisterButtonInputMonitor", on_x}, {"OH_GamePad_ButtonY_RegisterButtonInputMonitor", on_y},
        {"OH_GamePad_LeftShoulder_RegisterButtonInputMonitor", on_l}, {"OH_GamePad_RightShoulder_RegisterButtonInputMonitor", on_r},
        {"OH_GamePad_LeftTrigger_RegisterButtonInputMonitor", on_zl}, {"OH_GamePad_RightTrigger_RegisterButtonInputMonitor", on_zr},
        {"OH_GamePad_ButtonMenu_RegisterButtonInputMonitor", on_plus},
        {"OH_GamePad_Dpad_LeftButton_RegisterButtonInputMonitor", on_left}, {"OH_GamePad_Dpad_UpButton_RegisterButtonInputMonitor", on_up},
        {"OH_GamePad_Dpad_RightButton_RegisterButtonInputMonitor", on_right}, {"OH_GamePad_Dpad_DownButton_RegisterButtonInputMonitor", on_down},
        {"OH_GamePad_LeftThumbstick_RegisterButtonInputMonitor", on_ls}, {"OH_GamePad_RightThumbstick_RegisterButtonInputMonitor", on_rs}};
    static const char *const axis_monitors[] = {"OH_GamePad_LeftThumbstick_RegisterAxisInputMonitor", "OH_GamePad_RightThumbstick_RegisterAxisInputMonitor",
        "OH_GamePad_LeftTrigger_RegisterAxisInputMonitor", "OH_GamePad_RightTrigger_RegisterAxisInputMonitor", "OH_GamePad_Dpad_RegisterAxisInputMonitor"};
    int ok = 0, total = 0;
    for (unsigned i = 0; i < sizeof buttons / sizeof *buttons; i++, total++) {
        button_reg_fn reg; SYM(reg, buttons[i].name);
        if (reg && reg(buttons[i].cb) == GAME_CONTROLLER_SUCCESS) ok++;
        else fprintf(stderr, "[totk-ohos] controllers: %s failed\n", buttons[i].name);
    }
    for (unsigned i = 0; i < sizeof axis_monitors / sizeof *axis_monitors; i++, total++) {
        axis_reg_fn reg; SYM(reg, axis_monitors[i]);
        if (reg && reg(on_axis) == GAME_CONTROLLER_SUCCESS) ok++;
        else fprintf(stderr, "[totk-ohos] controllers: %s failed\n", axis_monitors[i]);
    }
    GameController_ErrorCode (*reg_device)(GameDevice_DeviceMonitorCallback); SYM(reg_device, "OH_GameDevice_RegisterDeviceMonitor");
    int monitor = reg_device && reg_device(on_device) == GAME_CONTROLLER_SUCCESS;
    /* the pads already connected: their count (names arrive with the next connection) */
    GameController_ErrorCode (*all)(GameDevice_AllDeviceInfos **); GameController_ErrorCode (*count)(const struct GameDevice_AllDeviceInfos *, int32_t *);
    GameController_ErrorCode (*all_destroy)(GameDevice_AllDeviceInfos **);
    SYM(all, "OH_GameDevice_GetAllDeviceInfos"); SYM(count, "OH_GameDevice_AllDeviceInfos_GetCount"); SYM(all_destroy, "OH_GameDevice_DestroyAllDeviceInfos");
    GameDevice_AllDeviceInfos *list = NULL; int32_t n = 0;
    GameController_ErrorCode (*get_info)(const struct GameDevice_AllDeviceInfos *, const int32_t, GameDevice_DeviceInfo **);
    SYM(get_info, "OH_GameDevice_AllDeviceInfos_GetDeviceInfo");
    if (all && count && all(&list) == GAME_CONTROLLER_SUCCESS && list) {
        int32_t total_devices = 0; if (count(list, &total_devices) != GAME_CONTROLLER_SUCCESS) total_devices = 0;
        for (int32_t i = 0; i < total_devices; i++) {   /* gamepads only (see on_device) */
            GameDevice_DeviceInfo *info = NULL; GameDevice_DeviceType dt = GAME_PAD; int pad = 1;
            if (get_info && p_info_type && get_info(list, i, &info) == GAME_CONTROLLER_SUCCESS && info) {
                if (p_info_type(info, &dt) == GAME_CONTROLLER_SUCCESS && dt == UNKNOWN) pad = 0;
                if (p_info_destroy) p_info_destroy(&info);
            }
            n += pad;
        }
        if (all_destroy) all_destroy(&list);
    }
#undef SYM
    pthread_mutex_lock(&mu);
    if (n > devices) devices = n;
    available = ok > 0;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "[totk-ohos] controllers: %d of %d monitors, device monitor %s, %d connected; face buttons %s\n", ok, total,
            monitor ? "on" : "unavailable", (int)n, by_label ? "by label (TOTK_FACE_BUTTONS=label)" : "by position (TOTK_FACE_BUTTONS=position)");
}

static int32_t stick(float v) {   /* the Android app's stick(): 12 % dead zone, rescaled to the Switch's range */
    const float dz = 0.12f; float m = v < 0 ? -v : v;
    if (m <= dz) return 0;
    if (m > 1) m = 1;
    int32_t r = (int32_t)((m - dz) / (1 - dz) * 32767.0f);
    return v < 0 ? -r : r;
}
/* The physical controllers' state for this read (a press shorter than one read still reaches the game once). */
__attribute__((visibility("hidden"))) int ohos_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&mu);
    out->buttons = keys | hat | trig | edges; edges = 0;
    out->lx = stick(axes[0]); out->ly = stick(axes[1]); out->rx = stick(axes[2]); out->ry = stick(axes[3]);
    out->connected = seen;
    pthread_mutex_unlock(&mu);
    return seen;
}
__attribute__((visibility("hidden"))) void ohos_gamepad_clear(void) { pthread_mutex_lock(&mu); clear_locked(); pthread_mutex_unlock(&mu); }
/* A controller key the kit has no monitor for, from the game page (Select / View: Minus). */
__attribute__((visibility("hidden"))) void totk_ohos_pad_key(int bit, int down) {
    if (bit < 0 || bit > SW_DOWN) return;
    pthread_mutex_lock(&mu); key_locked(bit, down); seen = 1; pthread_mutex_unlock(&mu);
}
/* Connected controllers (-1 when GameControllerKit is unavailable) and the last one's name. */
__attribute__((visibility("hidden"))) int totk_ohos_gamepad_status(char *name, size_t cap) {
    pthread_mutex_lock(&mu);
    int n = available > 0 ? devices : -1;
    if (cap) snprintf(name, cap, "%s", device_name);
    pthread_mutex_unlock(&mu);
    return n;
}
