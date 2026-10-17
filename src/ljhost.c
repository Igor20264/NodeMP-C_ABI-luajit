/* NodeMP language host for LuaJIT.
 *
 * type = "luajit" runs in an embedded LuaJIT state. Callbacks stay on the
 * server worker. node.job runs on one thread pinned to its own core.
 * One lock covers every state, so a long job stalls other LuaJIT callbacks
 * and leaves the server's own Lua free.
 */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <process.h>
#endif

#include "node.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#define MAX_RES 64
#define MAX_SLOT 2048
#define JOB_CAP 4096

#if defined(_WIN32)
#define STOP_GET() InterlockedCompareExchange(&g_stop, 0, 0)
#define STOP_SET() InterlockedExchange(&g_stop, 1)
static long g_stop;
#else
#define STOP_GET() __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)
#define STOP_SET() __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE)
static long g_stop;
#endif

typedef struct Res {
    int used;
    int alive;
    char name[96];
    lua_State *L;
} Res;

typedef struct Slot {
    int used;
    int res;
    int fn;
    int timer;
    int kind;
    int once;
} Slot;

typedef struct Job {
    int res;
    int fn;
    int arg;
    int done;
    struct Job *next;
} Job;

typedef struct Done {
    int res;
    int done;
    int result;
    int ok;
    char *error;
    struct Done *next;
} Done;

enum {
    KIND_BUILTIN = 1,
    KIND_REQUEST = 2,
    KIND_VEHICLE = 3,
    KIND_CLIENT = 4,
    KIND_TIMER = 5
};

static const char *const k_builtin[] = {
    "playerAuthenticated", "playerJoined", "playerLeft", "serverTick", "serverShutdown",
    "vehicleSpawned", "vehicleDeleted", "vehicleDamageChanged", "vehicleTagsChanged",
    "vehicleLockChanged", "vehiclePositionChanged", "vehicleInputsChanged",
    "vehicleElectricsChanged", "vehiclePowertrainChanged", "vehicleEngineChanged",
    "vehicleNodesChanged", "playerInputsChanged", "playerHeadPoseChanged",
    "vehicleTeleported", "vehicleBreakGroupsChanged", "playerCameraChanged",
    NULL
};

static const char *const k_request[] = {
    "playerConnectRequest", "vehicleSpawnRequest", "vehicleEnterRequest", "vehicleExitRequest",
    "vehicleCouplerRequest", "vehicleEditRequest", "vehiclePaintRequest", "vehicleTriggerRequest",
    "vehicleNodeGrabRequest", "queueJoinRequest", "queueAdmitRequest", "chatMessageRequest",
    NULL
};

static const char *const k_vehicle[] = {
    "vehicleEdited", "vehicleReset", "vehiclePainted", "playerSeatChanged",
    "playerVerifyReported", "vehicleCouplerChanged", "vehicleControllerChanged",
    "playerLagging", "playerRecovered", "resourceUnload", "queueAdmitted", "queueLeft",
    "queueNoShow", "playerJoinSlow",
    NULL
};

static const NodeApi *g_api;
static Res g_res[MAX_RES];
static Slot g_slot[MAX_SLOT];
static Job *g_head;
static Job *g_tail;
static int g_count;
static int g_lane_up;
static int g_core = -1;
static int g_ncpu;
static int g_reserve;
static unsigned long long g_reserve_log_at;
static Done *g_done_head;
static Done *g_done_tail;
static int g_done_armed;

#if defined(_WIN32)
static CRITICAL_SECTION g_vm;
static CRITICAL_SECTION g_q;
static CRITICAL_SECTION g_done_mu;
static CRITICAL_SECTION g_reserve_mu;
static CONDITION_VARIABLE g_cv;
static HANDLE g_thread;
static DWORD g_lane_tid;
#else
static pthread_mutex_t g_vm = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_q = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_reserve_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static pthread_t g_lane_thread;
static int g_lane_valid;
#if defined(__linux__)
static long g_lane_os_id;
#endif
#endif

static void log_jit(const char *msg) {
    if (!g_api || !msg) return;
    if (g_api->log_custom) g_api->log_custom("LuaJIT", 0xC4A35Au, msg);
    else if (g_api->log_info) g_api->log_info(msg);
}

static void log_err(const char *msg) {
    if (g_api && g_api->log_error && msg) g_api->log_error(msg);
}

static void vm_lock(void) {
#if defined(_WIN32)
    EnterCriticalSection(&g_vm);
#else
    pthread_mutex_lock(&g_vm);
#endif
}

static void vm_unlock(void) {
#if defined(_WIN32)
    LeaveCriticalSection(&g_vm);
#else
    pthread_mutex_unlock(&g_vm);
#endif
}

static void q_lock(void) {
#if defined(_WIN32)
    EnterCriticalSection(&g_q);
#else
    pthread_mutex_lock(&g_q);
#endif
}

static void q_unlock(void) {
#if defined(_WIN32)
    LeaveCriticalSection(&g_q);
#else
    pthread_mutex_unlock(&g_q);
#endif
}

static int q_wait_ms(unsigned ms) {
#if defined(_WIN32)
    return SleepConditionVariableCS(&g_cv, &g_q, ms) ? 1 : 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&g_cv, &g_q, &ts) == 0;
#endif
}

static void q_broadcast(void) {
#if defined(_WIN32)
    WakeAllConditionVariable(&g_cv);
#else
    pthread_cond_broadcast(&g_cv);
#endif
}

static unsigned long long tick_ms(void) {
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static int listed(const char *const *table, const char *name) {
    int i;
    for (i = 0; table[i]; i++) {
        if (strcmp(table[i], name) == 0) return 1;
    }
    return 0;
}

static Res *res_by_state(lua_State *L) {
    int i;
    for (i = 0; i < MAX_RES; i++) {
        if (g_res[i].used && g_res[i].L == L) return &g_res[i];
    }
    return NULL;
}

static Res *res_at(int index) {
    if (index < 0 || index >= MAX_RES || !g_res[index].used) return NULL;
    return &g_res[index];
}

static void own(const char *name) {
    if (g_api && g_api->set_resource_owner) g_api->set_resource_owner(name);
}

static int trusted_name(const char *name) {
    size_t need = offsetof(NodeApi, is_resource_trusted) + sizeof(void *);
    if (!g_api || g_api->struct_size < need || !g_api->is_resource_trusted || !name) return 0;
    return g_api->is_resource_trusted(name) ? 1 : 0;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = NULL;
    long size;
    char *buf;
#if defined(_WIN32)
    wchar_t wide[1024];
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)(sizeof wide / sizeof wide[0]));
    if (chars > 0) file = _wfopen(wide, L"rb");
#endif
    if (!file) file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    if (size && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return NULL;
    }
    buf[size] = '\0';
    fclose(file);
    *length = (size_t)size;
    return buf;
}

static int absolute_path(const char *path) {
    if (!path || !path[0]) return 0;
    if (path[0] == '/' || path[0] == '\\') return 1;
    return path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static int join_entry(char *out, size_t cap, const char *dir, const char *entry) {
    size_t dl;
    size_t el;
    int slash;
    if (!entry || !entry[0]) return 0;
    if (absolute_path(entry)) {
        if (strlen(entry) + 1 > cap) return 0;
        memcpy(out, entry, strlen(entry) + 1);
        return 1;
    }
    if (!dir) return 0;
    dl = strlen(dir);
    el = strlen(entry);
    slash = dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\');
    if (dl + (slash ? 0 : 1) + el + 1 > cap) return 0;
    memcpy(out, dir, dl);
    if (!slash) out[dl++] = '/';
    memcpy(out + dl, entry, el + 1);
    return 1;
}

static void log_reserve(int moved) {
    unsigned long long now;
    char buf[160];
    if (moved <= 0) return;
    now = tick_ms();
    if (g_reserve_log_at && now - g_reserve_log_at < 10000ull) return;
    g_reserve_log_at = now;
    snprintf(buf, sizeof buf, "moved %d thread(s) off core %d so the luajit lane keeps it", moved, g_core);
    log_jit(buf);
}

static void pin_this_thread(void) {
#if defined(_WIN32)
    GROUP_AFFINITY affinity;
    PROCESSOR_NUMBER ideal;
    if (g_core < 0) return;
    ZeroMemory(&affinity, sizeof affinity);
    affinity.Group = (WORD)(g_core / 64);
    affinity.Mask = ((DWORD_PTR)1) << (g_core % 64);
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, NULL)) {
        log_err("SetThreadGroupAffinity failed; the luajit lane is not pinned");
        g_core = -1;
        g_reserve = 0;
        return;
    }
    ZeroMemory(&ideal, sizeof ideal);
    ideal.Group = affinity.Group;
    ideal.Number = (BYTE)(g_core % 64);
    SetThreadIdealProcessorEx(GetCurrentThread(), &ideal, NULL);
#else
    cpu_set_t set;
    if (g_core < 0) return;
    CPU_ZERO(&set);
    CPU_SET((size_t)g_core, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        log_err("sched_setaffinity failed; the luajit lane is not pinned");
        g_core = -1;
        g_reserve = 0;
    }
#endif
}

static void name_this_thread(void) {
#if defined(_WIN32)
    typedef HRESULT(WINAPI * SetThreadDescriptionFn)(HANDLE, PCWSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    SetThreadDescriptionFn fn = kernel ? (SetThreadDescriptionFn)GetProcAddress(kernel, "SetThreadDescription") : NULL;
    if (fn) fn(GetCurrentThread(), L"nodemp-luajit");
#else
    pthread_setname_np(pthread_self(), "nodemp-luajit");
#endif
}

static void core_reserve(void) {
#if defined(_WIN32)
    HANDLE snap;
    THREADENTRY32 entry;
    int moved = 0;
    DWORD pid;
    DWORD_PTR bit;
    WORD group;
    if (!g_reserve || g_core < 0 || g_lane_tid == 0) return;
    EnterCriticalSection(&g_reserve_mu);
    bit = ((DWORD_PTR)1) << (g_core % 64);
    group = (WORD)(g_core / 64);
    pid = GetCurrentProcessId();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_reserve_mu);
        return;
    }
    entry.dwSize = sizeof entry;
    if (Thread32First(snap, &entry)) {
        do {
            HANDLE thread;
            GROUP_AFFINITY affinity;
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == g_lane_tid) continue;
            thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) continue;
            if (GetThreadGroupAffinity(thread, &affinity) && affinity.Group == group &&
                (affinity.Mask & bit) && affinity.Mask != bit) {
                affinity.Mask &= ~bit;
                if (SetThreadGroupAffinity(thread, &affinity, NULL)) moved++;
            }
            CloseHandle(thread);
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    log_reserve(moved);
    LeaveCriticalSection(&g_reserve_mu);
#elif defined(__linux__)
    DIR *dir;
    struct dirent *ent;
    int moved = 0;
    if (!g_reserve || g_core < 0 || g_lane_os_id == 0) return;
    pthread_mutex_lock(&g_reserve_mu);
    dir = opendir("/proc/self/task");
    if (!dir) {
        pthread_mutex_unlock(&g_reserve_mu);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        long tid = strtol(ent->d_name, &end, 10);
        cpu_set_t set;
        if (end == ent->d_name || tid <= 0 || tid == g_lane_os_id) continue;
        if (sched_getaffinity((pid_t)tid, sizeof set, &set) != 0) continue;
        if (!CPU_ISSET(g_core, &set) || CPU_COUNT(&set) <= 1) continue;
        CPU_CLR(g_core, &set);
        if (sched_setaffinity((pid_t)tid, sizeof set, &set) == 0) moved++;
    }
    closedir(dir);
    log_reserve(moved);
    pthread_mutex_unlock(&g_reserve_mu);
#else
    (void)0;
#endif
}

static int choose_core(void) {
    const char *env = getenv("NODEMP_LUAJIT_CORE");
    int ncpu = 1;
    int core;
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    ncpu = (int)info.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) ncpu = (int)n;
#endif
    if (ncpu < 1) ncpu = 1;
    g_ncpu = ncpu;
    if (env && env[0]) {
        char *end = NULL;
        if (strcmp(env, "last") == 0) return ncpu - 1;
        core = (int)strtol(env, &end, 10);
        if (end != env && *end == '\0' && core >= 0 && core < ncpu) return core;
    }
    if (ncpu > 3) return ncpu - 3;
    return ncpu - 1;
}

static Slot *slot_new(Res *res, int fn, int kind) {
    int i;
    for (i = 0; i < MAX_SLOT; i++) {
        if (!g_slot[i].used) {
            g_slot[i].used = 1;
            g_slot[i].res = (int)(res - g_res);
            g_slot[i].fn = fn;
            g_slot[i].timer = 0;
            g_slot[i].kind = kind;
            g_slot[i].once = 0;
            return &g_slot[i];
        }
    }
    return NULL;
}

static void slot_free(Slot *slot) {
    Res *res;
    if (!slot || !slot->used) return;
    res = res_at(slot->res);
    if (res && res->L && slot->fn != LUA_NOREF) luaL_unref(res->L, LUA_REGISTRYINDEX, slot->fn);
    slot->used = 0;
    slot->fn = LUA_NOREF;
    slot->timer = 0;
}

static int call_fn(Res *res, int fn, int nargs) {
    lua_State *L = res->L;
    if (!res->alive || !L) return -1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
    if (nargs > 0) lua_insert(L, -nargs - 1);
    if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0) {
        const char *text = lua_tostring(L, -1);
        char buf[512];
        snprintf(buf, sizeof buf, "%s: %s", res->name, text ? text : "lua error");
        log_err(buf);
        lua_pop(L, 1);
        return -1;
    }
    return lua_gettop(L);
}

static void on_builtin(int player_id, void *user) {
    Slot *slot = (Slot *)user;
    Res *res;
    if (!slot || !slot->used) return;
    vm_lock();
    res = res_at(slot->res);
    if (!res || !res->alive || !res->L) {
        vm_unlock();
        return;
    }
    lua_settop(res->L, 0);
    lua_pushinteger(res->L, player_id);
    call_fn(res, slot->fn, 1);
    lua_settop(res->L, 0);
    vm_unlock();
}

static void on_client(int player_id, const char *data, void *user) {
    Slot *slot = (Slot *)user;
    Res *res;
    if (!slot || !slot->used) return;
    vm_lock();
    res = res_at(slot->res);
    if (!res || !res->alive || !res->L) {
        vm_unlock();
        return;
    }
    lua_settop(res->L, 0);
    lua_pushinteger(res->L, player_id);
    lua_pushstring(res->L, data ? data : "");
    call_fn(res, slot->fn, 2);
    lua_settop(res->L, 0);
    vm_unlock();
}

static void on_vehicle(int player_id, int global_id, const char *data, void *user) {
    Slot *slot = (Slot *)user;
    Res *res;
    if (!slot || !slot->used) return;
    vm_lock();
    res = res_at(slot->res);
    if (!res || !res->alive || !res->L) {
        vm_unlock();
        return;
    }
    lua_settop(res->L, 0);
    lua_pushinteger(res->L, player_id);
    lua_pushinteger(res->L, global_id);
    lua_pushstring(res->L, data ? data : "");
    call_fn(res, slot->fn, 3);
    lua_settop(res->L, 0);
    vm_unlock();
}

static int on_request(int player_id, int global_id, const char *data, char *reason, int reason_cap, void *user) {
    Slot *slot = (Slot *)user;
    Res *res;
    int deny = 0;
    int top;
    if (!slot || !slot->used) return 0;
    vm_lock();
    res = res_at(slot->res);
    if (!res || !res->alive || !res->L) {
        vm_unlock();
        return 0;
    }
    lua_settop(res->L, 0);
    lua_pushinteger(res->L, player_id);
    lua_pushinteger(res->L, global_id);
    lua_pushstring(res->L, data ? data : "");
    top = call_fn(res, slot->fn, 3);
    if (top > 0) {
        int idx = lua_gettop(res->L);
        if (lua_type(res->L, idx) == LUA_TSTRING) {
            const char *text = lua_tostring(res->L, idx);
            deny = 1;
            if (reason && reason_cap > 1 && text) {
                snprintf(reason, (size_t)reason_cap, "%s", text);
            }
        } else if (lua_type(res->L, idx) == LUA_TBOOLEAN && !lua_toboolean(res->L, idx)) {
            deny = 1;
        }
    }
    lua_settop(res->L, 0);
    vm_unlock();
    return deny;
}

static void on_timer(int timer_id, void *user) {
    Slot *slot = (Slot *)user;
    Res *res;
    (void)timer_id;
    if (!slot || !slot->used) return;
    vm_lock();
    res = res_at(slot->res);
    if (!res || !res->alive || !res->L) {
        vm_unlock();
        return;
    }
    lua_settop(res->L, 0);
    call_fn(res, slot->fn, 0);
    lua_settop(res->L, 0);
    if (slot->once) slot_free(slot);
    vm_unlock();
}

static void deliver_done(int timer_id, void *user) {
    Done *list;
    Done *item;
    (void)timer_id;
    (void)user;
#if defined(_WIN32)
    EnterCriticalSection(&g_done_mu);
#else
    pthread_mutex_lock(&g_done_mu);
#endif
    list = g_done_head;
    g_done_head = NULL;
    g_done_tail = NULL;
    g_done_armed = 0;
#if defined(_WIN32)
    LeaveCriticalSection(&g_done_mu);
#else
    pthread_mutex_unlock(&g_done_mu);
#endif
    vm_lock();
    for (item = list; item; ) {
        Done *next = item->next;
        Res *res = res_at(item->res);
        if (res && res->alive && res->L) {
            lua_State *L = res->L;
            lua_settop(L, 0);
            if (item->done != LUA_NOREF) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, item->done);
                if (item->ok && item->result != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, item->result);
                else lua_pushnil(L);
                if (item->error) lua_pushstring(L, item->error);
                else lua_pushnil(L);
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    const char *text = lua_tostring(L, -1);
                    char buf[512];
                    snprintf(buf, sizeof buf, "%s: %s", res->name, text ? text : "job callback error");
                    log_err(buf);
                    lua_pop(L, 1);
                }
                luaL_unref(L, LUA_REGISTRYINDEX, item->done);
            }
            if (item->result != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, item->result);
            lua_settop(L, 0);
        }
        free(item->error);
        free(item);
        item = next;
    }
    vm_unlock();
}

static void arm_done(Done *done) {
    int arm = 0;
#if defined(_WIN32)
    EnterCriticalSection(&g_done_mu);
#else
    pthread_mutex_lock(&g_done_mu);
#endif
    if (g_done_tail) g_done_tail->next = done;
    else g_done_head = done;
    g_done_tail = done;
    if (!g_done_armed) {
        g_done_armed = 1;
        arm = 1;
    }
#if defined(_WIN32)
    LeaveCriticalSection(&g_done_mu);
#else
    pthread_mutex_unlock(&g_done_mu);
#endif
    if (arm && g_api && g_api->set_timeout) g_api->set_timeout(0, deliver_done, NULL);
}

static void run_job(Job *job) {
    Res *res;
    lua_State *L;
    Done *done;
    int rc;
    vm_lock();
    res = res_at(job->res);
    done = (Done *)calloc(1, sizeof *done);
    if (!done) {
        vm_unlock();
        return;
    }
    done->res = job->res;
    done->done = job->done;
    done->result = LUA_NOREF;
    if (!res || !res->alive || !res->L) {
        vm_unlock();
        done->ok = 0;
        done->error = strdup("resource unloaded");
        arm_done(done);
        return;
    }
    L = res->L;
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, job->fn);
    if (job->arg != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, job->arg);
    else lua_pushnil(L);
    rc = lua_pcall(L, 1, 1, 0);
    if (rc != 0) {
        const char *text = lua_tostring(L, -1);
        done->ok = 0;
        done->error = strdup(text ? text : "lua error");
        lua_pop(L, 1);
    } else {
        done->ok = 1;
        done->result = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, job->fn);
    if (job->arg != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, job->arg);
    lua_settop(L, 0);
    vm_unlock();
    arm_done(done);
}

#if defined(_WIN32)
static unsigned __stdcall lane_main(void *arg) {
#else
static void *lane_main(void *arg) {
#endif
    (void)arg;
    pin_this_thread();
    name_this_thread();
#if defined(_WIN32)
    g_lane_tid = GetCurrentThreadId();
#elif defined(__linux__)
    g_lane_thread = pthread_self();
    g_lane_valid = 1;
    g_lane_os_id = (long)syscall(SYS_gettid);
#else
    g_lane_thread = pthread_self();
    g_lane_valid = 1;
#endif
    core_reserve();
    q_lock();
    g_lane_up = 1;
    q_broadcast();
    for (;;) {
        Job *job;
        while (!STOP_GET() && !g_head) {
            if (q_wait_ms(5000)) continue;
            q_unlock();
            core_reserve();
            q_lock();
        }
        if (STOP_GET() && !g_head) break;
        job = g_head;
        g_head = job->next;
        if (!g_head) g_tail = NULL;
        g_count--;
        q_unlock();
        run_job(job);
        free(job);
        q_lock();
    }
    g_lane_up = 0;
    q_unlock();
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int enqueue(Job *job) {
    q_lock();
    if (!g_lane_up || STOP_GET() || g_count >= JOB_CAP) {
        q_unlock();
        return -1;
    }
    if (g_tail) g_tail->next = job;
    else g_head = job;
    g_tail = job;
    g_count++;
    q_broadcast();
    q_unlock();
    return 0;
}

static int l_log(lua_State *L) {
    int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
    int n;
    const char *text;
    if (lua_type(L, 1) == LUA_TTABLE) lua_remove(L, 1);
    n = lua_gettop(L);
    if (n > 1 && lua_isstring(L, 1)) {
        lua_getglobal(L, "string");
        lua_getfield(L, -1, "format");
        lua_remove(L, -2);
        lua_insert(L, 1);
        if (lua_pcall(L, n, 1, 0) != 0) {
            text = lua_tostring(L, -1);
            if (g_api && g_api->log_error) g_api->log_error(text ? text : "log format");
            return 0;
        }
    }
    text = n == 0 ? "" : lua_tostring(L, -1);
    if (!text) text = "";
    if (kind == 2 && g_api && g_api->log_warn) g_api->log_warn(text);
    else if (kind == 3 && g_api && g_api->log_error) g_api->log_error(text);
    else log_jit(text);
    return 0;
}

static int l_on(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    Res *res = res_by_state(L);
    Slot *slot;
    int fn;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!res) return luaL_error(L, "node.on outside a resource");
    lua_pushvalue(L, 2);
    fn = luaL_ref(L, LUA_REGISTRYINDEX);
    slot = slot_new(res, fn, KIND_CLIENT);
    if (!slot) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn);
        return luaL_error(L, "too many handlers");
    }
    own(res->name);
    if (listed(k_request, name)) {
        slot->kind = KIND_REQUEST;
        g_api->register_cancellable_event(name, on_request, slot);
    } else if (listed(k_vehicle, name)) {
        slot->kind = KIND_VEHICLE;
        g_api->register_vehicle_event(name, on_vehicle, slot);
    } else if (listed(k_builtin, name)) {
        slot->kind = KIND_BUILTIN;
        g_api->register_builtin_event(name, on_builtin, slot);
    } else {
        slot->kind = KIND_CLIENT;
        g_api->register_client_event(name, on_client, slot);
    }
    own(NULL);
    lua_pushinteger(L, (lua_Integer)(slot - g_slot) + 1);
    return 1;
}

static int l_timer(lua_State *L) {
    int once = (int)lua_tointeger(L, lua_upvalueindex(1));
    lua_Integer ms = luaL_checkinteger(L, 1);
    Res *res = res_by_state(L);
    Slot *slot;
    int fn;
    int id;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 0) ms = 0;
    if (!res) return luaL_error(L, "timer outside a resource");
    lua_pushvalue(L, 2);
    fn = luaL_ref(L, LUA_REGISTRYINDEX);
    slot = slot_new(res, fn, KIND_TIMER);
    if (!slot) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn);
        return luaL_error(L, "too many handlers");
    }
    own(res->name);
    if (once) id = g_api->set_timeout((uint32_t)ms, on_timer, slot);
    else id = g_api->set_interval((uint32_t)ms, on_timer, slot);
    own(NULL);
    if (id < 0) {
        slot_free(slot);
        return luaL_error(L, "timer was refused");
    }
    slot->once = once;
    slot->timer = id;
    lua_pushinteger(L, id);
    return 1;
}

static int l_cancel(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    int i;
    if (g_api && g_api->clear_timer) g_api->clear_timer(id);
    for (i = 0; i < MAX_SLOT; i++) {
        if (g_slot[i].used && g_slot[i].kind == KIND_TIMER && g_slot[i].timer == id) {
            g_slot[i].timer = 0;
            slot_free(&g_slot[i]);
            break;
        }
    }
    return 0;
}

static int l_job(lua_State *L) {
    Res *res = res_by_state(L);
    Job *job;
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (!res) return luaL_error(L, "node.job outside a resource");
    job = (Job *)calloc(1, sizeof *job);
    if (!job) return luaL_error(L, "out of memory");
    job->res = (int)(res - g_res);
    job->arg = LUA_NOREF;
    job->done = LUA_NOREF;
    lua_pushvalue(L, 1);
    job->fn = luaL_ref(L, LUA_REGISTRYINDEX);
    if (!lua_isnoneornil(L, 2)) {
        lua_pushvalue(L, 2);
        job->arg = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (lua_isfunction(L, 3)) {
        lua_pushvalue(L, 3);
        job->done = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (enqueue(job) != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, job->fn);
        if (job->arg != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, job->arg);
        if (job->done != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, job->done);
        free(job);
        return luaL_error(L, "luajit core lane is not accepting jobs");
    }
    return 0;
}

static char *api_bytes(int (*fn)(char *, int)) {
    int n;
    char *buf;
    int got;
    if (!fn) return NULL;
    n = fn(NULL, 0);
    if (n < 0) n = 256 * 1024;
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) return NULL;
    got = fn(buf, n + 1);
    if (got < 0) {
        free(buf);
        buf = (char *)malloc(1024 * 1024);
        if (!buf) return NULL;
        got = fn(buf, 1024 * 1024);
        if (got < 0) {
            free(buf);
            return NULL;
        }
    }
    buf[got] = '\0';
    return buf;
}

static int push_text(lua_State *L, int (*fn)(char *, int)) {
    char *text = api_bytes(fn);
    if (!text) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, text);
    free(text);
    return 1;
}

static int l_snapshot(lua_State *L) {
    return push_text(L, g_api->get_world_snapshot_json);
}

static int l_players(lua_State *L) {
    size_t need = offsetof(NodeApi, get_players_json) + sizeof(void *);
    if (!g_api || g_api->struct_size < need || !g_api->get_players_json) {
        lua_pushnil(L);
        return 1;
    }
    return push_text(L, g_api->get_players_json);
}

static int l_player_name(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    char buf[512];
    int n;
    if (!g_api || !g_api->get_player_name) {
        lua_pushnil(L);
        return 1;
    }
    n = g_api->get_player_name(id, buf, (int)sizeof buf);
    if (n < 0) lua_pushnil(L);
    else lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

static int l_player_count(lua_State *L) {
    lua_pushinteger(L, g_api && g_api->get_player_count ? g_api->get_player_count() : 0);
    return 1;
}

static int l_vehicle_count(lua_State *L) {
    lua_pushinteger(L, g_api && g_api->get_vehicle_count ? g_api->get_vehicle_count() : 0);
    return 1;
}

static int l_uptime(lua_State *L) {
    lua_pushnumber(L, g_api && g_api->get_server_uptime ? g_api->get_server_uptime() : 0);
    return 1;
}

static int l_stats(lua_State *L) {
    int reset = lua_toboolean(L, 1);
    size_t need = offsetof(NodeApi, get_handler_stats_json) + sizeof(void *);
    int (*fn)(char *, int);
    char *text;
    int n;
    char stack[4096];
    if (!g_api || g_api->struct_size < need || !g_api->get_handler_stats_json) {
        lua_pushnil(L);
        return 1;
    }
    n = g_api->get_handler_stats_json(reset, stack, (int)sizeof stack);
    if (n >= 0 && n < (int)sizeof stack) {
        lua_pushlstring(L, stack, (size_t)n);
        return 1;
    }
    fn = NULL;
    (void)fn;
    text = NULL;
    if (n < 0) n = 65536;
    text = (char *)malloc((size_t)n + 1);
    if (!text) {
        lua_pushnil(L);
        return 1;
    }
    n = g_api->get_handler_stats_json(reset, text, n + 1);
    if (n < 0) {
        free(text);
        lua_pushnil(L);
        return 1;
    }
    text[n] = '\0';
    lua_pushlstring(L, text, (size_t)n);
    free(text);
    return 1;
}

static int l_spawn(lua_State *L) {
    const char *config = luaL_checkstring(L, 1);
    const char *owner = luaL_optstring(L, 2, "luajit");
    int id = g_api && g_api->spawn_vehicle ? g_api->spawn_vehicle(config, owner) : -1;
    lua_pushinteger(L, id);
    return 1;
}

static int l_delete(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    int rc = g_api && g_api->delete_vehicle ? g_api->delete_vehicle(id) : -1;
    lua_pushinteger(L, rc);
    return 1;
}

static int l_version(lua_State *L) {
    lua_pushstring(L, LUAJIT_VERSION);
    return 1;
}

static int l_core(lua_State *L) {
    lua_newtable(L);
    lua_pushinteger(L, g_core);
    lua_setfield(L, -2, "index");
    lua_pushinteger(L, g_ncpu);
    lua_setfield(L, -2, "processors");
    lua_pushboolean(L, g_lane_up);
    lua_setfield(L, -2, "pinned");
    return 1;
}

static void push_log(lua_State *L, int kind) {
    lua_pushinteger(L, kind);
    lua_pushcclosure(L, l_log, 1);
}

static void open_node(lua_State *L) {
    lua_newtable(L);
    lua_newtable(L);
    push_log(L, 2);
    lua_setfield(L, -2, "warn");
    push_log(L, 3);
    lua_setfield(L, -2, "error");
    lua_newtable(L);
    push_log(L, 1);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "log");
    push_log(L, 2);
    lua_setfield(L, -2, "warn");
    push_log(L, 3);
    lua_setfield(L, -2, "error");
    lua_pushcfunction(L, l_on);
    lua_setfield(L, -2, "on");
    lua_pushinteger(L, 1);
    lua_pushcclosure(L, l_timer, 1);
    lua_setfield(L, -2, "after");
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, l_timer, 1);
    lua_setfield(L, -2, "every");
    lua_pushcfunction(L, l_cancel);
    lua_setfield(L, -2, "cancel");
    lua_pushcfunction(L, l_job);
    lua_setfield(L, -2, "job");
    lua_pushcfunction(L, l_snapshot);
    lua_setfield(L, -2, "snapshot");
    lua_pushcfunction(L, l_players);
    lua_setfield(L, -2, "players");
    lua_pushcfunction(L, l_player_name);
    lua_setfield(L, -2, "player_name");
    lua_pushcfunction(L, l_player_count);
    lua_setfield(L, -2, "player_count");
    lua_pushcfunction(L, l_vehicle_count);
    lua_setfield(L, -2, "vehicle_count");
    lua_pushcfunction(L, l_uptime);
    lua_setfield(L, -2, "uptime");
    lua_pushcfunction(L, l_stats);
    lua_setfield(L, -2, "handler_stats");
    lua_pushcfunction(L, l_spawn);
    lua_setfield(L, -2, "spawn_vehicle");
    lua_pushcfunction(L, l_delete);
    lua_setfield(L, -2, "delete_vehicle");
    lua_pushcfunction(L, l_version);
    lua_setfield(L, -2, "version");
    lua_pushcfunction(L, l_core);
    lua_setfield(L, -2, "core_info");
    lua_setglobal(L, "node");
}

static void lock_down(lua_State *L) {
    lua_pushnil(L);
    lua_setglobal(L, "io");
    lua_pushnil(L);
    lua_setglobal(L, "debug");
    lua_pushnil(L);
    lua_setglobal(L, "ffi");
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "loaded");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            lua_setfield(L, -2, "ffi");
        }
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_setfield(L, -2, "loadlib");
        lua_pushstring(L, "");
        lua_setfield(L, -2, "cpath");
    }
    lua_pop(L, 1);
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        lua_newtable(L);
        lua_getfield(L, -2, "clock");
        lua_setfield(L, -2, "clock");
        lua_getfield(L, -2, "time");
        lua_setfield(L, -2, "time");
        lua_getfield(L, -2, "difftime");
        lua_setfield(L, -2, "difftime");
        lua_getfield(L, -2, "date");
        lua_setfield(L, -2, "date");
        lua_setglobal(L, "os");
    }
    lua_pop(L, 1);
}

static void set_path(lua_State *L, const char *dir) {
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushfstring(L, "%s\\?.lua;%s\\server\\?.lua;%s\\?.lua", dir, dir, dir);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
}

static int host_load(const char *name, const char *dir, const char *entry, void *user) {
    char path[1024];
    char *source;
    size_t length = 0;
    Res *res = NULL;
    lua_State *L;
    int i;
    int trust;
    char banner[160];
    (void)user;
    if (!entry || !entry[0]) {
        log_err("luajit resource has no server.main");
        return -1;
    }
    if (!join_entry(path, sizeof path, dir, entry)) {
        log_err("luajit resource path is too long");
        return -1;
    }
    source = read_file(path, &length);
    if (!source) {
        char banner[1100];
        snprintf(banner, sizeof banner, "luajit resource file could not be read: %s", path);
        log_err(banner);
        return -1;
    }
    vm_lock();
    for (i = 0; i < MAX_RES; i++) {
        if (!g_res[i].used) {
            res = &g_res[i];
            break;
        }
    }
    if (!res) {
        vm_unlock();
        free(source);
        log_err("too many luajit resources");
        return -1;
    }
    memset(res, 0, sizeof *res);
    L = luaL_newstate();
    if (!L) {
        vm_unlock();
        free(source);
        log_err("luaL_newstate failed");
        return -1;
    }
    luaL_openlibs(L);
    trust = trusted_name(name);
    if (!trust) lock_down(L);
    set_path(L, dir);
    open_node(L);
    lua_newtable(L);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, dir);
    lua_setfield(L, -2, "dir");
    lua_pushboolean(L, trust);
    lua_setfield(L, -2, "trusted");
    lua_setglobal(L, "resource");
    res->used = 1;
    res->alive = 1;
    res->L = L;
    snprintf(res->name, sizeof res->name, "%s", name);
    if (luaL_loadbuffer(L, source, length, path) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        const char *text = lua_tostring(L, -1);
        snprintf(banner, sizeof banner, "%s: %s", name, text ? text : "load failed");
        log_err(banner);
        lua_close(L);
        res->L = NULL;
        res->alive = 0;
        res->used = 0;
        vm_unlock();
        free(source);
        return -1;
    }
    lua_settop(L, 0);
    vm_unlock();
    free(source);
    snprintf(banner, sizeof banner, "%s loaded%s", name, trust ? " (trusted)" : "");
    log_jit(banner);
    return 0;
}

static void drop_jobs(int index) {
    Job *prev = NULL;
    Job *job;
    q_lock();
    job = g_head;
    while (job) {
        Job *next = job->next;
        if (job->res == index) {
            if (prev) prev->next = next;
            else g_head = next;
            if (g_tail == job) g_tail = prev;
            g_count--;
            free(job);
        } else {
            prev = job;
        }
        job = next;
    }
    q_unlock();
}

static void host_unload(const char *name, void *user) {
    int i;
    (void)user;
    vm_lock();
    for (i = 0; i < MAX_RES; i++) {
        if (!g_res[i].used || strcmp(g_res[i].name, name) != 0) continue;
        g_res[i].alive = 0;
        if (g_res[i].L) lua_close(g_res[i].L);
        g_res[i].L = NULL;
        g_res[i].used = 0;
        {
            int s;
            for (s = 0; s < MAX_SLOT; s++) {
                if (g_slot[s].used && g_slot[s].res == i) {
                    g_slot[s].used = 0;
                    g_slot[s].fn = LUA_NOREF;
                }
            }
        }
        vm_unlock();
        drop_jobs(i);
        return;
    }
    vm_unlock();
}

static int start_lane(void) {
    char banner[192];
    g_core = choose_core();
    g_reserve = g_core >= 0 && getenv("NODEMP_LUAJIT_RESERVE") && strcmp(getenv("NODEMP_LUAJIT_RESERVE"), "0") != 0;
    if (!getenv("NODEMP_LUAJIT_RESERVE")) g_reserve = g_core >= 0;
#if defined(_WIN32)
    InitializeCriticalSection(&g_vm);
    InitializeCriticalSection(&g_q);
    InitializeCriticalSection(&g_done_mu);
    InitializeCriticalSection(&g_reserve_mu);
    InitializeConditionVariable(&g_cv);
    g_thread = (HANDLE)_beginthreadex(NULL, 0, lane_main, NULL, 0, NULL);
    if (!g_thread) return -1;
#else
    if (pthread_create(&g_thread, NULL, lane_main, NULL) != 0) return -1;
#endif
    q_lock();
    while (!g_lane_up && !STOP_GET()) q_wait_ms(1000);
    q_unlock();
    snprintf(banner, sizeof banner,
        "resources with type = \"luajit\" run on %s; background lane is pinned to core %d",
        LUAJIT_VERSION, g_core);
    log_jit(banner);
    return g_lane_up ? 0 : -1;
}

NODE_PLUGIN_ABI()

NODE_EXPORT int node_plugin_init(const NodeApi *api) {
    static NodeLanguageHost host;
    g_api = api;
    if (!api || (api->abi_version >> 16) != NODE_ABI_VERSION_MAJOR) return -1;
    if (api->struct_size < (uint32_t)(offsetof(NodeApi, register_language_host) + sizeof(void *))) {
        log_err("ljhost needs register_language_host");
        return -1;
    }
    if (start_lane() != 0) {
        log_err("luajit lane did not start");
        return -1;
    }
    memset(&host, 0, sizeof host);
    host.struct_size = (uint32_t)sizeof host;
    host.type_name = "luajit";
    host.load = host_load;
    host.unload = host_unload;
    if (api->register_language_host(&host) != 0) {
        log_err("type \"luajit\" is already taken");
        return -1;
    }
    return 0;
}

NODE_EXPORT void node_plugin_shutdown(void) {
    int i;
    STOP_SET();
    q_broadcast();
#if defined(_WIN32)
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
#else
    if (g_lane_valid) pthread_join(g_thread, NULL);
#endif
    vm_lock();
    for (i = 0; i < MAX_RES; i++) {
        if (g_res[i].L) lua_close(g_res[i].L);
        g_res[i].L = NULL;
        g_res[i].used = 0;
        g_res[i].alive = 0;
    }
    vm_unlock();
}
