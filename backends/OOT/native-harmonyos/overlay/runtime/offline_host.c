/* Host side of the services a game reaches only when it has a network or a console identity: the host is an offline
   console. Network requests are never accepted, so a game keeps its offline paths (no sockets, friends or local wireless
   follow). From games/wonder (Super Mario Bros. Wonder imports them); here for the Switch 2 games (GAME_NS2: Donkey Kong
   Bananza initializes nifm at boot). TOTK and BotW import none of them and do not build this file. */
#include "runtime.h"
void hle_signal_system_event(void *event);
#include "game_version.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>
#include "host_os.h"   /* host_sha256_* */
#include "host_wait.h"   /* runtime_host_wait_*: the virtual core goes while a socket call blocks */

/* Failures in each service's own module; games test them for success only. */
#define R_NIFM_UNAVAILABLE (110u | (300u << 9))
#define R_FRIENDS_UNAVAILABLE (121u | (6u << 9))
#define R_LDN_UNAVAILABLE (203u | (32u << 9))
#define R_SSL_UNAVAILABLE (123u | (204u << 9))
#define R_NSD_UNAVAILABLE (141u | (2u << 9))

/* ---------- nn::oe ---------- */
/* The host never asks the game to exit, so a section that defers exit requests has nothing to defer. */
static void enter_exit_section(void) { }
static void leave_exit_section(void) { }
static void display_version(char *out) { memset(out, 0, 16); strncpy(out, TOTK_GAME_VERSION, 15); }
/* Stable for this install and this game, like the console's per-application pseudo device id: the app's own files folder
   (TOTK_PROFILE_DIR, set by the HarmonyOS frontend; the Mac took its host UUID) and the title. */
static void pseudo_device_id(uint8_t *uuid) {
    const char *where = getenv("TOTK_PROFILE_DIR"); uint8_t digest[32]; uint64_t title = GAME_TITLE_ID;
    host_sha256_ctx c; host_sha256_init(&c);
    if (where) host_sha256_update(&c, where, strlen(where));
    host_sha256_update(&c, &title, sizeof title); host_sha256_final(&c, digest);
    memcpy(uuid, digest, 16);
}

/* ---------- nn::nifm ---------- */
static uint32_t nifm_initialize(void) { return 0; }
static unsigned char nifm_available(void) { return 0; }
static void nifm_local_mode(unsigned char on) { (void)on; }
static void nifm_submit_and_wait(void) { fprintf(stderr, "[totk] network request: refused (offline host)\n"); }
static void nifm_cancel(void) { }
static uint32_t nifm_unavailable(void) { return R_NIFM_UNAVAILABLE; }

/* ---------- nn::friends: no friends and no play history on an offline host ---------- */
static void friends_initialize(void) { }
static void friends_allocator(void *a, void *f) { (void)a; (void)f; }
static uint32_t friends_check(unsigned char *available, const void *uid) { (void)uid; if (available) *available = 0; return 0; }
static uint32_t friends_unavailable(void) { return R_FRIENDS_UNAVAILABLE; }
static uint32_t friends_list(int *count, void *out, const void *uid, int offset, int max, const void *filter) {
    (void)out; (void)uid; (void)offset; (void)max; (void)filter; if (count) *count = 0; return 0;
}
static uint32_t friends_add_history(void) { return 0; }

/* ---------- nn::ldn (local wireless): there is no radio, so no station or access point opens ---------- */
static uint32_t ldn_unavailable(void) { return R_LDN_UNAVAILABLE; }
static void ldn_finalize(void) { }
static int ldn_state(void) { return 0; }   /* State_None */
static void ldn_attach_event(void *event) { if (!event) runtime_fail("ldn state event"); hle_bind_system_event(event, 0); }

/* ---------- nn::ssl, nn::nsd, nn::ngc ---------- */
static uint32_t ssl_initialize(void) { return 0; }
static uint32_t ssl_unavailable(void) { return R_SSL_UNAVAILABLE; }
static uint32_t nsd_environment(char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); return 0; }
static uint32_t ngc_initialize(void *self, void *work, uint64_t size, unsigned char check) { (void)self; (void)work; (void)size; (void)check; return 0; }
/* Nothing is filtered: every word is reported clean. */
static uint32_t ngc_check(uint32_t *masks, uint32_t count, const void **words, uint64_t n) {
    (void)words; (void)n; if (masks) memset(masks, 0, count * sizeof *masks); return 0;
}

static uint32_t nsd_unavailable(void) { return R_NSD_UNAVAILABLE; }

/* ---------- nn::socket: the library starts, but there is no network, so every socket call fails with ENETDOWN ---------- */
#define SOCKET_ENETDOWN 50   /* Horizon uses the BSD errno values */
#define SOCKET_EAI_FAIL 4
static _Thread_local int socket_errno;
static uint32_t socket_initialize(void) { return 0; }
static int socket_down(void) { socket_errno = SOCKET_ENETDOWN; return -1; }
static int socket_last_error(void) { return socket_errno; }
static int socket_addr_info(const char *node, const char *service, const void *hints, void **out) {
    (void)node; (void)service; (void)hints; if (out) *out = NULL; return SOCKET_EAI_FAIL;
}
static void socket_free_addr_info(void *info) { (void)info; }
static uint32_t socket_htonl(uint32_t v) { return htonl(v); }
static uint16_t socket_htons(uint16_t v) { return htons(v); }
static const char *socket_ntop(int family, const void *in, char *out, uint32_t size) { return family == 2 ? inet_ntop(AF_INET, in, out, size) : NULL; }
/* EventFd is a local descriptor of the bsd service and works with no network (seedds/nxnative hle_net.c, MIT; the Z-A
   port's za_hle.c): gRPC, which nn::npln links (Ocarina of Time at startup, right after mounting _npln:), wakes its pollers
   with one; failed with ENETDOWN, its poll engine's factory returned null and grpc_init called abort. Host counters with
   Horizon's flags (bit 0 reads one at a time, bits 1 and 2 never block). Sockets still fail, so a poll only ever has these
   to watch; any other descriptor has nothing to say. An endless wait comes back with 0 after 100 ms. */
enum { EFD_BASE = 0x4000, EFD_MAX = 64, SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4, SK_EBADF = 9, SK_EAGAIN = 35 };
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
static struct { uint64_t count; int used, flags; } g_efd[EFD_MAX];
static pthread_mutex_t g_efd_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_efd_c = PTHREAD_COND_INITIALIZER;
static int efd_used(int fd) { return fd >= EFD_BASE && fd < EFD_BASE + EFD_MAX && g_efd[fd - EFD_BASE].used; }
static uint64_t efd_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void efd_wait(int64_t ns) {   /* with g_efd_m held */
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t v = (uint64_t)dl.tv_nsec + (uint64_t)ns; dl.tv_sec += (time_t)(v / 1000000000ull); dl.tv_nsec = (long)(v % 1000000000ull);
    pthread_cond_timedwait(&g_efd_c, &g_efd_m, &dl);
}
static int efd_create(uint64_t v, int flags) {
    pthread_mutex_lock(&g_efd_m);
    for (int i = 0; i < EFD_MAX; i++) if (!g_efd[i].used) { g_efd[i].used = 1; g_efd[i].count = v; g_efd[i].flags = flags; pthread_mutex_unlock(&g_efd_m); return EFD_BASE + i; }
    pthread_mutex_unlock(&g_efd_m);
    fprintf(stderr, "[offline] socket: more than %d event descriptors\n", EFD_MAX); socket_errno = 24; return -1;   /* EMFILE */
}
static int efd_close(int fd) {
    pthread_mutex_lock(&g_efd_m);
    int ok = efd_used(fd);
    if (ok) { g_efd[fd - EFD_BASE].used = 0; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m);
    if (!ok) { socket_errno = SK_EBADF; return -1; }
    return 0;
}
/* The blocking calls below give up the thread's virtual core while they wait (runtime_host_wait, before any host lock):
   gRPC's poller thread sits in Poll most of the time, and holding its core there starved the game threads bound to that
   core (Ocarina of Time stopped at its first frame, three threads waiting for core 2). */
static int efd_read(int fd, uint64_t *out) {
    runtime_host_wait hw = {0}; runtime_host_wait_begin(&hw);
    pthread_mutex_lock(&g_efd_m);
    int r = -1;
    for (;;) {
        if (!efd_used(fd)) { socket_errno = SK_EBADF; break; }
        uint64_t *c = &g_efd[fd - EFD_BASE].count; int f = g_efd[fd - EFD_BASE].flags;
        if (*c) { if (f & 1) { if (out) *out = 1; *c -= 1; } else { if (out) *out = *c; *c = 0; } r = 0; break; }
        if (f & 6) { socket_errno = SK_EAGAIN; break; }
        efd_wait(100000000);
    }
    pthread_mutex_unlock(&g_efd_m);
    runtime_host_wait_end(&hw);
    return r;
}
static int efd_write(int fd, uint64_t v) {
    pthread_mutex_lock(&g_efd_m);
    int ok = efd_used(fd);
    if (ok) { g_efd[fd - EFD_BASE].count += v; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m);
    if (!ok) { socket_errno = SK_EBADF; return -1; }
    return 0;
}
static int efd_ready(sk_pollfd *p, uint64_t n) {   /* with g_efd_m held */
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        p[i].revents = 0;
        if (p[i].fd < EFD_BASE || p[i].fd >= EFD_BASE + EFD_MAX) continue;
        if (!efd_used(p[i].fd)) { p[i].revents = 0x20; ready++; continue; }   /* POLLNVAL: closed while waiting */
        if (g_efd[p[i].fd - EFD_BASE].count) p[i].revents |= p[i].events & (SK_POLLIN | SK_POLLRDNORM);
        p[i].revents |= p[i].events & (SK_POLLOUT | SK_POLLWRNORM);
        if (p[i].revents) ready++;
    }
    return ready;
}
static int socket_poll(void *fds, uint64_t n, int timeout) {
    sk_pollfd *p = fds;
    int64_t limit = (timeout < 0 ? 100 : (int64_t)timeout) * 1000000;
    uint64_t start = efd_now_ns();
    runtime_host_wait hw = {0}; if (timeout) runtime_host_wait_begin(&hw);
    pthread_mutex_lock(&g_efd_m);
    int ready;
    while (!(ready = efd_ready(p, n))) {
        int64_t left = limit - (int64_t)(efd_now_ns() - start);
        if (left <= 0) break;
        efd_wait(left);
    }
    pthread_mutex_unlock(&g_efd_m);
    if (timeout) runtime_host_wait_end(&hw);
    return ready;
}
/* Select over no host descriptor: nothing becomes ready; the timeout is waited out (100 ms for an endless one) */
static int socket_select(int nfds, void *r, void *w, void *e, const int64_t *tv) {
    (void)nfds; (void)r; (void)w; (void)e;
    int64_t ms = tv ? tv[0] * 1000 + tv[1] / 1000 : 100;
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    if (ms > 0) { runtime_host_wait hw = {0}; runtime_host_wait_begin(&hw); nanosleep(&t, NULL); runtime_host_wait_end(&hw); }
    return 0;
}
static int socket_eventfd(uint64_t v, int flags) { return efd_create(v, flags); }
static int socket_eventfd_read(int fd, uint64_t *v) { return efd_read(fd, v); }
static int socket_eventfd_write(int fd, uint64_t v) { return efd_write(fd, v); }
static int socket_close(int fd) { return efd_close(fd); }
static void socket_set_errno(int e) { socket_errno = e; }

/* ---------- nn::crypto ---------- */
static void random_bytes(void *out, uint64_t n) {
    for (uint8_t *p = out; n; ) { ssize_t k = getrandom(p, n > 256 ? 256 : (size_t)n, 0); if (k <= 0) runtime_fail("getrandom"); p += k; n -= (uint64_t)k; }
}

/* ---------- play reports: nothing is reported from an offline host ---------- */
static uint32_t ok_result(void) { return 0; }
static void nothing(void) { }
static int socket_ready(void) { return 1; }
static uint32_t false_flag(void) { return 0; }
static void friends_event(void *self, void *event) { (void)self; if (event) { hle_bind_system_event(event, 0); hle_signal_system_event(event); } }
static uint8_t invitation_event[0x80] __attribute__((aligned(16)));   /* friend invitations: bound, never signalled */
static void *friends_invitation_event(void) { static int bound; if (!bound) { bound = 1; hle_bind_system_event(invitation_event, 0); } return invitation_event; }
static uint32_t friends_done(void *self, unsigned char *done) { (void)self; if (done) *done = 1; return 0; }

/* ---------- nn::settings ---------- */
/* The SDK builds the language's code through the settings service; the table is the system's fixed one. */
static unsigned char language_is(const int *language, const char *code) {
    static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                        "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    if (!language || !code || *language < 0 || *language >= (int)(sizeof codes / sizeof *codes)) return 0;
    return !strncmp(codes[*language], code, 8);
}

void *runtime_offline_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", enter_exit_section},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", leave_exit_section},
        {"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", display_version},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", pseudo_device_id}, {"_ZN2nn4nifm10InitializeEv", nifm_initialize},
        {"_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available}, {"_ZN2nn4nifm19SetLocalNetworkModeEb", nifm_local_mode},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit_and_wait},
        {"_ZN2nn4nifm20CancelNetworkRequestEv", nifm_cancel},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_unavailable}, {"_ZN2nn6socket10InitializeEPvmmi", socket_initialize},
        {"_ZN2nn6socket12GetLastErrorEv", socket_last_error},
        /* SDK 20's socket forms (Pokemon Pokopia): initialization succeeds, every socket call finds the network down. */
        {"_ZN2nn6socket10InitializeERKNS0_6ConfigE", socket_initialize}, {"_ZN2nn6socket8FinalizeEv", socket_initialize},
        {"_ZN2nn6socket12GetLastErrnoEv", socket_last_error},
        /* More of SDK 20's offline surface (Pokemon Pokopia): sockets stay down; crash reports, play reports and the profanity
           filter's setup are accepted; SSL, friends, local wireless, data delivery and the network request report the
           network unavailable, so the game keeps to its offline paths. */
        {"_ZN2nn6socket13IsInitializedEv", socket_ready}, {"_ZN2nn6socket4ReadEiPvm", socket_down}, {"_ZN2nn6socket5WriteEiPKvm", socket_down},
        {"_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", socket_down},
        {"_ZN2nn2oe17EnableCrashReportEv", nothing}, {"_ZN2nn2oe18DisableCrashReportEv", nothing},
        {"_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", nothing},
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", nothing}, {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", false_flag},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_unavailable},
        {"_ZN2nn3ssl7ContextC1Ev", nothing}, {"_ZN2nn3ssl7ContextD1Ev", nothing}, {"_ZN2nn3ssl7Context7DestroyEv", ok_result},
        {"_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ssl_unavailable}, {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_unavailable},
        {"_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ssl_unavailable}, {"_ZN2nn3ssl8FinalizeEv", ok_result},
        {"_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn7friends12AsyncContextC1Ev", nothing}, {"_ZN2nn7friends12AsyncContextD1Ev", nothing},
        {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", friends_event}, {"_ZN2nn7friends12AsyncContext7HasDoneEPb", friends_done},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileEPKNS_7account23NetworkServiceAccountIdEi", friends_unavailable},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", friends_unavailable},
        {"_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", friends_unavailable},
        {"_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", friends_unavailable},
        {"_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", false_flag},
        {"_ZN2nn7friends7ProfileC1Ev", nothing},
        {"_ZN2nn7friends36GetFriendInvitationNotificationEventEv", friends_invitation_event},
        {"_ZN2nn3ldn10DisconnectEv", ldn_unavailable}, {"_ZN2nn3ldn12CloseStationEv", ldn_unavailable}, {"_ZN2nn3ldn16CloseAccessPointEv", ldn_unavailable},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_unavailable},
        {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", ldn_unavailable}, {"_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_unavailable},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_unavailable},
        {"_ZN2nn3ngc15ProfanityFilterC1Ev", nothing}, {"_ZN2nn3ngc15ProfanityFilterD1Ev", nothing}, {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ok_result},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ok_result}, {"_ZN2nn3ngc15ProfanityFilter22SetNormalizationOptionEi", nothing},
        {"_ZN2nn5prepo10PlayReportC1EPKc", nothing}, {"_ZN2nn5prepo10PlayReportD1Ev", nothing},
        {"_ZN2nn5prepo10PlayReport3AddEPKcRKNS0_5ArrayE", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcb", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcf", ok_result},
        {"_ZN2nn5prepo5ArrayC1Ev", nothing}, {"_ZN2nn5prepo5Array9SetBufferEPvm", nothing}, {"_ZN2nn5prepo5Array3AddEf", ok_result}, {"_ZN2nn5prepo5Array3AddEl", ok_result},
        {"_ZN2nn4bcat17DeliveryCacheFileC1Ev", nothing}, {"_ZN2nn4bcat17DeliveryCacheFileD1Ev", nothing},
        {"_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", nifm_unavailable}, {"_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", nifm_unavailable},
        {"_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev", nothing}, {"_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", nothing},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", nifm_unavailable},
        {"_ZN2nn4bcat21DeliveryCacheProgressC1Ev", nothing}, {"_ZN2nn4bcat21DeliveryCacheProgressD1Ev", nothing},
        {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", nifm_unavailable},
        {"_ZN2nn6socket10GetSockOptEiiiPvPj", socket_down}, {"_ZN2nn6socket10SetSockOptEiiiPKvj", socket_down},
        {"_ZN2nn6socket11GetSockNameEiP8sockaddrPj", socket_down}, {"_ZN2nn6socket4PollEP6pollfdmi", socket_poll},
        {"_ZN2nn6socket4RecvEiPvmi", socket_down}, {"_ZN2nn6socket4SendEiPKvmi", socket_down}, {"_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", socket_down},
        {"_ZN2nn6socket6AcceptEiP8sockaddrPj", socket_down}, {"_ZN2nn6socket6ListenEii", socket_down},
        {"_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", socket_select}, {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", socket_down},
        {"_ZN2nn6socket7ConnectEiPK8sockaddrj", socket_down}, {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", socket_down},
        {"_ZN2nn6socket8ShutdownEii", socket_down},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", socket_addr_info},
        {"_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", socket_free_addr_info}, {"_ZN2nn6socket9InetHtonlEj", socket_htonl},
        {"_ZN2nn6socket9InetHtonsEt", socket_htons},
        {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", socket_down},
        {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", socket_down},
        {"_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", socket_down},
        {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", socket_down},
        {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", socket_down}, {"_ZN2nn6socket5CloseEi", socket_close},
        {"_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", socket_eventfd}, {"_ZN2nn6socket11EventFdReadEiPm", socket_eventfd_read},
        {"_ZN2nn6socket12EventFdWriteEim", socket_eventfd_write},
        {"_ZN2nn6socket4PollEPNS0_6PollFdEmi", socket_poll}, {"nnsocketPoll", socket_poll},
        {"nnsocketClose", socket_close}, {"nnsocketGetLastErrno", socket_last_error}, {"nnsocketSetLastErrno", socket_set_errno},
        {"nnsocketSocket", socket_down}, {"nnsocketBind", socket_down}, {"nnsocketConnect", socket_down}, {"nnsocketListen", socket_down},
        {"nnsocketAccept", socket_down}, {"nnsocketGetPeerName", socket_down}, {"nnsocketGetSockName", socket_down},
        {"nnsocketGetSockOpt", socket_down}, {"nnsocketSetSockOpt", socket_down}, {"nnsocketFcntl", socket_down},
        {"nnsocketShutdown", socket_down}, {"nnsocketSend", socket_down}, {"nnsocketRecv", socket_down},
        {"nnsocketSendMsg", socket_down}, {"nnsocketRecvMsg", socket_down},
        {"nnsocketResolverGetOption", socket_down}, {"nnsocketResolverSetOption", socket_down},
        {"nnsocketGetAddrInfo", socket_addr_info}, {"nnsocketFreeAddrInfo", socket_free_addr_info},
        {"_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", random_bytes},
        {"_ZN2nn5prepo10PlayReportC1Ev", nothing}, {"_ZN2nn5prepo10PlayReport10SetEventIdEPKc", ok_result},
        {"_ZN2nn5prepo10PlayReport9SetBufferEPvm", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcl", ok_result},
        {"_ZN2nn5prepo10PlayReport3AddEPKcS3_", ok_result}, {"_ZN2nn6socket6SocketEiii", socket_down},
        {"_ZN2nn6socket4BindEiPK8sockaddrj", socket_down},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", socket_addr_info},
        {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo", socket_free_addr_info}, {"_ZN2nn6socket8InetNtopEiPKvPcj", socket_ntop},
        {"_ZN2nn6socket9InetNtohsEt", socket_htons}, {"_ZN2nn7friends10InitializeEv", friends_initialize},
        {"_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", friends_allocator},
        {"_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", friends_check},
        {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", friends_unavailable},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", friends_list},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_unavailable},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", friends_add_history},
        {"_ZN2nn3ldn10InitializeEv", ldn_unavailable}, {"_ZN2nn3ldn8FinalizeEv", ldn_finalize},
        {"_ZN2nn3ldn8GetStateEv", ldn_state},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_event},
        {"_ZN2nn3ldn15OpenAccessPointEv", ldn_unavailable}, {"_ZN2nn3ldn11OpenStationEv", ldn_unavailable},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_unavailable},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_unavailable},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ssl10InitializeEv", ssl_initialize}, {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_unavailable},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", nsd_environment},
        {"_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", language_is},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ngc_initialize},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
