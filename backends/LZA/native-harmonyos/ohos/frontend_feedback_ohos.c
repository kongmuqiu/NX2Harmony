/* Rumble and motion with the phone's own vibrator and sensors, for the game's HD rumble and six-axis sensor
   (runtime/hid_feedback.c: on unless TOTK_RUMBLE=0 / TOTK_GYRO=0; the settings page sets them). GameControllerKit has
   no vibration or motion API, so a connected controller does not rumble and its own gyroscope is not read.
   Rumble: runtime_rumble_set keeps the game's latest value per side. A thread of ours turns them into one strength
   (rumble_mix.h, times TOTK_RUMBLE_GAIN) and, as the phone's vibrator has no strength of its own here
   (OH_Vibrator_PlayVibration takes a duration only), plays it as a pulse of that share of every 50 ms: full strength
   overlaps the pulses into one long vibration. A value the game has not repeated for 250 ms counts as silence.
   Motion: the accelerometer and gyroscope at 200 Hz, turned into the frame the game's sensor uses (x the screen's
   right, y its top, z out of the face; acceleration in G with gravity pointing down; rad/s), as the Android app's
   MotionAxes does: the phone's sensors keep its natural (portrait) orientation, so x and y are turned by the display's
   quarter turns. TOTK_OHOS_GYRO_FLIP=1 adds a half turn, for a landscape the other way round; TOTK_GYRO_AXES remaps as
   on the Mac. runtime/motion_fusion.c makes the game's states from the samples. The sensors run only while the game
   page is in the foreground (ohos_feedback_foreground). */
#include "runtime.h"
#include "rumble_mix.h"
#include <pthread.h>
#include <sensors/oh_sensor.h>
#include <sensors/vibrator.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <window_manager/oh_display_manager.h>

/* ---- rumble ---- */
enum { TICK_MS = 50 };
static pthread_mutex_t r_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t r_cv = PTHREAD_COND_INITIALIZER;
static float want[2][4] = {{0, 160, 0, 320}, {0, 160, 0, 320}};   /* r_mu */
static uint64_t want_at[2];                                       /* r_mu */
static pthread_once_t r_once = PTHREAD_ONCE_INIT;
static atomic_int rumble_off;

static void *rumble_thread(void *arg) {
    (void)arg;
    float gain = rumble_gain_parse(getenv("TOTK_RUMBLE_GAIN"));
    const Vibrator_Attribute attr = {.vibratorId = 0, .usage = VIBRATOR_USAGE_MEDIA};
    fprintf(stderr, "[rumble] phone vibrator (gain %.2f)\n", gain);
    int playing = 0, first = 1;
    pthread_mutex_lock(&r_mu);
    for (;;) {
        uint64_t now = runtime_now_ns();
        int a = rumble_amplitude(want[0], want_at[0], now, gain), b = rumble_amplitude(want[1], want_at[1], now, gain);
        int amp = a > b ? a : b;
        if (!amp) {
            if (playing) { pthread_mutex_unlock(&r_mu); OH_Vibrator_Cancel(); pthread_mutex_lock(&r_mu); playing = 0; }
            pthread_cond_wait(&r_cv, &r_mu);   /* nothing to play: only the game's next value can change that */
            continue;
        }
        int on = (amp * TICK_MS + 127) / 255;
        if (on < 10) on = 10;
        int duration = on >= TICK_MS - 8 ? TICK_MS + 25 : on;
        pthread_mutex_unlock(&r_mu);
        int rc = OH_Vibrator_PlayVibration(duration, attr);
        if (first) { fprintf(stderr, "[rumble] first vibration: strength %d of 255, %d ms pulses: %d\n", amp, duration, rc); first = 0; }
        if (rc == PERMISSION_DENIED || rc == UNSUPPORTED) {
            fprintf(stderr, "[rumble] the vibrator refused (%d); rumble off\n", rc);
            atomic_store(&rumble_off, 1);
            return NULL;
        }
        pthread_mutex_lock(&r_mu);
        playing = 1;
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += TICK_MS * 1000000L; if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        while (pthread_cond_timedwait(&r_cv, &r_mu, &until) == 0) {}   /* the game's values arrive every few ms: keep the tick */
    }
    return NULL;
}
static void rumble_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, rumble_thread, NULL)) fprintf(stderr, "[rumble] thread could not start; rumble off\n");
    else pthread_detach(t);
}
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    if (side > 1 || atomic_load(&rumble_off)) return;
    pthread_once(&r_once, rumble_start);
    pthread_mutex_lock(&r_mu);
    want[side][0] = al; want[side][1] = fl; want[side][2] = ah; want[side][3] = fh; want_at[side] = runtime_now_ns();
    pthread_cond_signal(&r_cv);
    pthread_mutex_unlock(&r_mu);
}
void runtime_feedback_quiet(void) {   /* the page left the foreground: stop now, not after the stale time */
    pthread_mutex_lock(&r_mu); want_at[0] = want_at[1] = 0; pthread_cond_signal(&r_cv); pthread_mutex_unlock(&r_mu);
}

/* ---- motion ---- */
static pthread_mutex_t m_mu = PTHREAD_MUTEX_INITIALIZER;
static Sensor_Subscriber *subscriber;
static Sensor_SubscriptionId *id_acc, *id_gyro;
static Sensor_SubscriptionAttribute *attribute;
static int subscribed, flip;
static float acc[3];
static atomic_int have_acc, quarter_turns;
static atomic_uint samples;

static void turn(double x, double y, double z, int q, double out[3]) {
    switch (q & 3) {
        case 1: out[0] = -y; out[1] = x; break;
        case 2: out[0] = -x; out[1] = -y; break;
        case 3: out[0] = y; out[1] = -x; break;
        default: out[0] = x; out[1] = y; break;
    }
    out[2] = z;
}
static void read_rotation(void) {
    NativeDisplayManager_Rotation r = DISPLAY_MANAGER_ROTATION_0;
    if (OH_NativeDisplayManager_GetDefaultDisplayRotation(&r) == DISPLAY_MANAGER_OK) atomic_store(&quarter_turns, (int)r);
}
static void on_sensor(Sensor_Event *event) {
    Sensor_Type type; float *d = NULL; uint32_t n = 0;
    if (OH_SensorEvent_GetType(event, &type) != SENSOR_SUCCESS || OH_SensorEvent_GetData(event, &d, &n) != SENSOR_SUCCESS || !d || n < 3) return;
    if (type == SENSOR_TYPE_ACCELEROMETER) { acc[0] = d[0]; acc[1] = d[1]; acc[2] = d[2]; atomic_store(&have_acc, 1); return; }
    if (type != SENSOR_TYPE_GYROSCOPE || !atomic_load(&have_acc)) return;
    if (atomic_fetch_add(&samples, 1) % 200 == 0) read_rotation();   /* once a second: the screen may have turned */
    int q = atomic_load(&quarter_turns) + (flip ? 2 : 0);
    const double g = 9.80665;
    double a[3], w[3];
    turn(-acc[0] / g, -acc[1] / g, -acc[2] / g, q, a);   /* the sensor reports the push against gravity: negate */
    turn(d[0], d[1], d[2], q, w);
    runtime_motion_feed(a, w, runtime_now_ns());
}
static void motion_start_locked(void) {
    if (subscribed || !runtime_gyro_enabled()) return;
    static int axes_read;
    if (!axes_read) {
        axes_read = 1;
        const char *axes = getenv("TOTK_GYRO_AXES"); int r = runtime_motion_axes(axes);
        if (r < 0) fprintf(stderr, "[gyro] TOTK_GYRO_AXES=%s is not three of x, y, z (each once, e.g. \"-x,y,-z\"); ignored\n", axes);
        if (r > 0) fprintf(stderr, "[gyro] axes remapped by TOTK_GYRO_AXES=%s\n", axes);
        const char *f = getenv("TOTK_OHOS_GYRO_FLIP"); flip = f && !strcmp(f, "1");
    }
    if (!subscriber) {
        subscriber = OH_Sensor_CreateSubscriber(); id_acc = OH_Sensor_CreateSubscriptionId(); id_gyro = OH_Sensor_CreateSubscriptionId();
        attribute = OH_Sensor_CreateSubscriptionAttribute();
        if (!subscriber || !id_acc || !id_gyro || !attribute) { fprintf(stderr, "[gyro] sensor objects could not be created; motion off\n"); return; }
        OH_SensorSubscriber_SetCallback(subscriber, on_sensor);
        OH_SensorSubscriptionId_SetType(id_acc, SENSOR_TYPE_ACCELEROMETER);
        OH_SensorSubscriptionId_SetType(id_gyro, SENSOR_TYPE_GYROSCOPE);
        OH_SensorSubscriptionAttribute_SetSamplingInterval(attribute, 5000000);   /* ns: 200 Hz */
    }
    read_rotation();
    atomic_store(&have_acc, 0);
    int ra = OH_Sensor_Subscribe(id_acc, attribute, subscriber), rg = ra == SENSOR_SUCCESS ? OH_Sensor_Subscribe(id_gyro, attribute, subscriber) : -1;
    if (ra != SENSOR_SUCCESS || rg != SENSOR_SUCCESS) {
        if (ra == SENSOR_SUCCESS) OH_Sensor_Unsubscribe(id_acc, subscriber);
        fprintf(stderr, "[gyro] sensors refused (accelerometer %d, gyroscope %d); motion off\n", ra, rg);
        return;
    }
    subscribed = 1;
    runtime_motion_live(1);
    fprintf(stderr, "[gyro] phone sensors on, display %d quarter turns%s\n", atomic_load(&quarter_turns), flip ? ", flipped" : "");
}
static void motion_stop_locked(void) {
    if (!subscribed) return;
    OH_Sensor_Unsubscribe(id_gyro, subscriber);
    OH_Sensor_Unsubscribe(id_acc, subscriber);
    subscribed = 0;
    runtime_motion_live(0);
    fprintf(stderr, "[gyro] phone sensors off\n");
}

/* totk_ohos_start: the sensors for the game's motion (when it has them on). */
__attribute__((visibility("hidden"))) void ohos_feedback_start(void) {
    pthread_mutex_lock(&m_mu); motion_start_locked(); pthread_mutex_unlock(&m_mu);
}
/* The game page went to the background (0) or came back (1). */
__attribute__((visibility("hidden"))) void ohos_feedback_foreground(int on) {
    if (!on) runtime_feedback_quiet();
    pthread_mutex_lock(&m_mu);
    if (on) motion_start_locked(); else motion_stop_locked();
    pthread_mutex_unlock(&m_mu);
}
