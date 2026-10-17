/* node.h - Node native module SDK (C ABI).
 *
 * ============================================================================
 *  WRITING A MODULE
 * ============================================================================
 *
 * A module is a shared library (.dll on Windows, .so elsewhere) placed in the
 * "modules/" folder next to the server executable. It must export exactly
 * three symbols (the loader looks these names up with GetProcAddress/dlsym):
 *
 *   NODE_EXPORT uint32_t node_plugin_abi(void);             // NODE_ABI_VERSION
 *   NODE_EXPORT int  node_plugin_init(const NodeApi* api);  // 0 = success,
 *                                                           // nonzero = refuse load
 *   NODE_EXPORT void node_plugin_shutdown(void);
 *
 * Use the NODE_PLUGIN_ABI() macro below for the first one.
 *
 * The NodeApi pointer passed to node_plugin_init stays valid until after
 * node_plugin_shutdown returns.
 *
 * ============================================================================
 *  ABI COMPATIBILITY
 * ============================================================================
 *
 * NodeApi is a positional struct: a module reads each capability at a fixed
 * offset decided at COMPILE time. If the server reorders or inserts a field,
 * a module built against the older layout does not fail -- it silently calls
 * whatever now sits at that offset. That is unrecoverable and undebuggable, so
 * the layout is a contract:
 *
 *   - node_plugin_abi() is read FIRST, before node_plugin_init is called and
 *     before any field of NodeApi is touched. A module whose major differs
 *     from the server's, or that does not export the symbol at all (i.e. was
 *     built against a pre-versioning SDK), is refused with a clear message
 *     instead of being allowed to crash the server.
 *   - New capabilities are APPENDED. Nothing is reordered, nothing is removed;
 *     a retired call becomes a stub that keeps its slot.
 *   - Appending bumps the minor and keeps the major: an older module still
 *     finds everything it knows about where it expects it. Any change that
 *     moves an existing field bumps the major.
 *   - Major 2 did exactly that, once: the six pg_* entries were deleted rather
 *     than stubbed, because the database left the server entirely (it is the
 *     `db` module now). Every module built against a 1.x header must be
 *     rebuilt; nothing else about the layout changed.
 *   - abi_version and struct_size are the first two members, so a module can
 *     verify them before trusting the rest, and so struct_size tells it how
 *     much of the struct the server actually filled in.
 *
 * BUILD: the SDK is this single self-contained C header - no import library,
 * no server internals. Any C or C++ toolchain works; see
 * plugins/plugin-example/CMakeLists.txt for a minimal CMake project (build
 * with a static CRT on Windows so the DLL has no runtime dependency).
 *
 * ============================================================================
 *  RETURN CONVENTIONS
 * ============================================================================
 *
 * - A function that performs an action returns 0 on success and -1 on failure.
 *   A function that answers a question returns 1 for yes and 0 for no. Both are
 *   int; read the doc of the one you are calling.
 * - A function that fills a buffer (char* buf, int buflen) returns the number
 *   of bytes written, or -1 if the buffer is too small.
 * - ASKING FOR THE SIZE: call it with buf = NULL (or buflen = 0) and it returns
 *   the length it would write, WITHOUT the NUL terminator, so allocate
 *   length + 1. A -1 from a size query means the thing does not exist, not
 *   that it is large. Do this for anything that can grow -- the world snapshot,
 *   the metrics blob, a vehicle config -- rather than guessing a buffer:
 *
 *       int n = api->get_world_snapshot_json(NULL, 0);
 *       if (n >= 0) {
 *           char* buf = malloc((size_t)n + 1);
 *           if (api->get_world_snapshot_json(buf, n + 1) >= 0) { ... }
 *           free(buf);
 *       }
 *
 * ============================================================================
 *  THREADING MODEL
 * ============================================================================
 *
 * - Every NodeApi function is thread-safe and may be called from any thread,
 *   including threads the module spawns itself.
 * - Unless noted otherwise, calls run INLINE on the calling thread (queries
 *   snapshot server state under internal locks; emits/kicks perform the
 *   network write directly).
 * - All registered callbacks (client events, builtin events, verdict
 *   callbacks, timers, job done-callbacks, HTTP callbacks, database
 *   completions (pg_*), resource-bus callbacks) are invoked on the single
 *   framework worker thread, one at a time - two callbacks never run
 *   concurrently with each other. Blocking inside a callback stalls every
 *   Lua resource and module handler, so keep them short and hand long work
 *   to submit_job or your own threads.
 * - Exceptions that do NOT run on the worker thread:
 *     - the log sink (set_log_sink) runs inline on WHATEVER thread produced
 *       the log line, see its doc below;
 *     - the relay filter (register_relay_filter) runs inline on network
 *       threads, possibly under internal server locks - it must be a fast,
 *       pure function of module data. Never call state-mutating NodeApi
 *       functions (seat_player, kick_player, spawn_vehicle, ...) from it;
 *     - the job work function (submit_job) runs on a background pool
 *       thread.
 *
 * ============================================================================
 *  THE WIRE PROTOCOL
 * ============================================================================
 *
 * The wire protocol (server/include/net/Protocol.h) is owned by the server:
 * modules emit custom events through emit_client/emit_all (binary EVENT
 * frames carrying a name and an opaque payload) and never touch packet
 * framing themselves. Event names starting with "node:" are reserved for
 * the framework; internal deliveries (e.g. client-resource streaming) ride
 * their own typed packets, never events.
 */

#ifndef NODE_H
#define NODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version of this header. The major changes only when the layout of
 * NodeApi changes in a way that moves an existing field; appending a new
 * capability bumps the minor, which older modules survive unchanged. The
 * loader compares majors and refuses a mismatch. */
#define NODE_ABI_VERSION_MAJOR 2u
#define NODE_ABI_VERSION_MINOR 8u
#define NODE_ABI_VERSION ((NODE_ABI_VERSION_MAJOR << 16) | NODE_ABI_VERSION_MINOR)

/* Log levels passed to a NodeLogSink. */
#define NODE_LOG_INFO 0
#define NODE_LOG_WARN 1
#define NODE_LOG_ERROR 2
#define NODE_LOG_DEBUG 3

/* seat_player `seat_kind`: which seat to force a player into. */
#define NODE_SEAT_DRIVER 0
#define NODE_SEAT_PASSENGER 1

/* set_vehicle_lock / get_vehicle_lock `mode` (wire v12): who may CLAIM a
 * seat. The whitelist passed alongside is always allowed regardless of mode.
 * Locks gate client seat claims only (the claimant gets a DeniedLocked seat
 * verdict): nobody already seated is evicted, and the server-driven
 * seat_player bypasses locks like it bypasses the request hooks. */
#define NODE_LOCK_UNLOCKED 0 /* anyone may enter (default) */
#define NODE_LOCK_ALL 1 /* nobody may enter in any role except the whitelist */
#define NODE_LOCK_DRIVER_ONLY 2 /* the DRIVER seat needs the whitelist; passengers ride freely */

/* Seat role reported by get_player_vehicle / get_vehicle_occupants and by the
 * "playerSeatChanged" notification (matches the wire SeatRole; NOTE this is a
 * DIFFERENT numbering from seat_player's seat_kind above). */
#define NODE_ROLE_NONE 0
#define NODE_ROLE_DRIVER 1
#define NODE_ROLE_PASSENGER 2

/* player_verify `level` (ABI 1.12): how thoroughly the player's launcher
 * helper re-checks the game install -- the wire's VerifyLevel. Size, scripts
 * and full are cumulative; strict builds on scripts (not full) and adds the
 * archive tables of contents, the game root and the user folder. */
#define NODE_VERIFY_SIZE 1 /* every manifest entry's length */
#define NODE_VERIFY_SCRIPTS 2 /* plus a hash of the game's lua/ and ui/ trees */
#define NODE_VERIFY_FULL 3 /* hash everything, archives included (slow: an audit) */
#define NODE_VERIFY_STRICT 4 /* archive tables of contents against the server's reference manifest, the whole game root, the user folder */

/* Callback for custom client events (client sent an EVENT frame with this
 * name). data is a NUL-terminated string (the raw event payload). Runs on
 * the framework worker thread. */
typedef void (*NodeClientEventCallback)(int player_id, const char* data, void* user);

/* Callback for builtin events (register_builtin_event). Runs on the framework
 * worker thread. This is the OBSERVER form: it is notified but cannot change
 * anything. Builtin names:
 *   "playerAuthenticated" - player identified, before resource sync
 *   "playerJoined"     - player finished syncing and is fully in the session
 *   "playerLeft"       - player disconnected (the reason: register_vehicle_event,
 *                        data; "Replaced" = a new login of its account)
 *   "serverTick"       - fired every 100 ms; player_id is -1
 *   "serverShutdown"   - fired once during server shutdown; player_id is -1
 *   "vehicleSpawned"   - a vehicle entered the world; player_id carries the
 *                        vehicle globalId (not a player id)
 *   "vehicleDeleted"   - a vehicle left the world; player_id carries the
 *                        vehicle globalId (not a player id)
 *   "vehicleDamageChanged" - the sync authority reported a new damage version
 *                        for a vehicle; player_id carries the vehicle
 *                        globalId. Compare with get_vehicle_damage_counter
 *                        and offer a resync.
 *   "vehicleTagsChanged" - a server plugin set or removed a tag on a vehicle
 *                        (wire v11); player_id carries the vehicle globalId.
 *                        Read the new set with get_vehicle_tags_json.
 *   "vehicleLockChanged" - a server plugin changed a vehicle's lock (wire
 *                        v12); player_id carries the vehicle globalId. Read
 *                        the new state with get_vehicle_lock.
 *   Streams as events (2026-09-10), coalesced to one pending dispatch per
 *   (event, id) and skipped without subscribers; the argument is the vehicle
 *   or player id, read the data with the matching getter:
 *   "vehiclePositionChanged", "vehicleInputsChanged", "vehicleElectricsChanged",
 *   "vehiclePowertrainChanged", "vehicleEngineChanged", "vehicleNodesChanged",
 *   "playerInputsChanged", "playerHeadPoseChanged";
 *   rare transitions, one event per packet: "vehicleTeleported",
 *   "vehicleBreakGroupsChanged", "playerCameraChanged".
 *
 * The cancellable request builtins below can ALSO be observed with
 * register_builtin_event (player_id = the requesting player); an observer
 * cannot deny. To veto, use register_cancellable_event instead. The
 * vehicleEdited/vehicleReset/vehiclePainted notifications carry a vehicle
 * globalId and a data payload that this one-int callback cannot express -
 * observe them with register_vehicle_event (see NodeVehicleEventCallback).
 *
 * NAMING (server 1.2.0). Notifications read <subject><Verb-ed>
 * ("playerJoined", "vehicleSpawned"); requests a handler can deny read
 * <subject><Action>Request ("playerConnectRequest"); the relay filter is
 * "relayRequest". The names used before 1.2.0 -- "playerJoin",
 * "playerConnecting", "onShutdown", every "on...Request", "canRelay" -- are
 * GONE. They were deprecated aliases in 1.2.0 and are no longer accepted:
 * every register_*_event entry refuses an old name and logs
 *   event "playerJoin" was renamed to "playerJoined"; nothing is subscribed
 *   to the old name (module <file>)
 * ONCE per module per old name for the life of the server process (the
 * module is the .dll/.so whose node_plugin_init is running, or the owner set
 * with set_resource_owner). The Lua prelude keeps its own once-table per
 * resource state, so a Lua resource is told once per load -- again after a
 * reload -- while node.raw.on/off and this C layer never repeat a line.
 * Refusing rather than ignoring matters: any other string is a legal WIRE
 * event name, so a silently accepted old name would register a handler for
 * a client event nobody sends. For the same reason a client may not send an
 * event under an old name either. */
typedef void (*NodeBuiltinEventCallback)(int player_id, void* user);

/* Callback for the notification events (register_vehicle_event). Runs on the
 * framework worker thread. OBSERVER form: it is notified after the server
 * already relayed and cached the action - there is nothing to cancel (the
 * initiating client simulates its own vehicle, so the action has already
 * happened locally for it) - or judged a report the player's launcher sent
 * ("playerVerifyReported", global_id -1). Return value: none.
 *   "vehicleEdited"  - a client changed a vehicle's config/parts.
 *                      global_id = the vehicle, data = the config JSON.
 *   "vehicleReset"   - a client reset/recovered a vehicle.
 *                      global_id = the vehicle, data = the pos/rot JSON.
 *   "vehiclePainted" - a client repainted a vehicle.
 *                      global_id = the vehicle, data = the paints JSON.
 *   "playerSeatChanged" - a player's seat/role changed (entered/left a seat,
 *                      or a server seat/unseat/swap/grace takeover moved it).
 *                      player_id = the player, global_id = the vehicle it now
 *                      occupies (-1 when it went on foot), data = the new role
 *                      ("driver"/"passenger"/"none"). Observe it to track
 *                      occupancy without decoding PlayerVehicle packets.
 *   "playerVerifyReported" - the player's launcher helper reported a
 *                      game-install check (ABI 1.12): the one every join
 *                      runs, and every mid-session one (player_verify, or
 *                      the helper's own schedule). player_id = the player,
 *                      global_id = -1, data = {"level":"size"|"scripts"|
 *                      "full"|"strict","outcome":"clean"|"differs"|"failed",
 *                      "problems":N,"detail":"...","manifest":"<hash>"}
 *                      (manifest is "" unless the level is strict). Fired
 *                      AFTER the server's own verdict: a differs or failed
 *                      report has already kicked the player when [integrity]
 *                      verify is not off.
 *   "resourceUnload" - (-1, -1, reason): the resource that registered the
 *                      handler is about to be unloaded (server 1.2.0);
 *                      reason is "reload" or "shutdown". Dispatched to that
 *                      resource's handlers only, synchronously, right before
 *                      its state goes -- a native module is never a
 *                      resource, so only a hosted resource (one a language
 *                      host loaded, registering under its owner) ever
 *                      receives it.
 *   The join queue (ABI 2.8): "queueAdmitted" (player_id = the new player,
 *                      data = {"accountId","name","waitedSec"}), "queueLeft"
 *                      and "queueNoShow" (-1, -1, JSON naming the account),
 *                      "playerJoinSlow" (player_id = the joining player).
 * player_id is the initiating client; data is NUL-terminated and valid only
 * for the duration of the call. A deletion is observed through the plain
 * "vehicleDeleted" builtin instead (register_builtin_event). */
typedef void (*NodeVehicleEventCallback)(int player_id, int global_id, const char* data, void* user);

/* Verdict callback for the cancellable request builtins
 * (register_cancellable_event). Runs on the framework worker thread while the
 * requesting client's network thread waits for the verdict - keep it fast.
 * Return 0 to ALLOW, nonzero to DENY. Every registered handler runs; one veto
 * denies. Optionally write a NUL-terminated human-readable reason (up to
 * reason_cap bytes, including the NUL) into `reason`; it is delivered to the
 * player only where the wire carries it (playerConnectRequest kicks the
 * player with it), and is logged server-side otherwise.
 *
 * Cancellable builtin names, with what player_id / global_id / data carry
 * (the pre-1.2.0 spelling with an "on" prefix -- "onPlayerConnectRequest" --
 * is refused, see NodeBuiltinEventCallback):
 *   "playerConnectRequest" - a player finished the handshake (name/id known,
 *                        before any content or resource streaming).
 *                        global_id = -1, data = the player's name.
 *                        DENY kicks the player with `reason`.
 *   "vehicleSpawnRequest" - a client asks to spawn a vehicle.
 *                        global_id = the requested gid (0 for a brand-new
 *                        vehicle), data = the spawn config JSON.
 *                        DENY answers a targeted SpawnDeny; the client removes
 *                        the just-created local vehicle.
 *   "vehicleEnterRequest" / "vehicleExitRequest" - a client asks to take
 *                        or leave a seat. global_id = the vehicle, data = "".
 *                        DENY answers a targeted EnterDeny/ExitDeny.
 *   "vehicleCouplerRequest" - a client actuates a coupler/door group (any
 *                        client, even a non-owner). global_id = the vehicle,
 *                        data = the trigger controller-call JSON. DENY means
 *                        the door actuates for nobody.
 *   "vehicleEditRequest" - the driver asks to change a vehicle's config/
 *                        parts (wire v10). global_id = the vehicle, data =
 *                        the config JSON. The edit is optimistic on the
 *                        initiator, so a DENY sends it the PREVIOUS full
 *                        cached state back (targeted Resync) - the local
 *                        change rolls back, damage included.
 *   "vehiclePaintRequest" - the driver asks to repaint (wire v10).
 *                        global_id = the vehicle, data = the paints JSON.
 *                        DENY sends the previous paints back (targeted
 *                        Paint) so the optimistic repaint reverts.
 *   "vehicleTriggerRequest" - a client asks the vehicle's SYNC AUTHORITY
 *                        to execute a trigger (wire v12, Vehicle::TriggerReq).
 *                        global_id = the vehicle, data = the controller-call
 *                        JSON. DENY drops the request silently.
 *   "vehicleNodeGrabRequest" - EXPERIMENTAL node grabber (wire v12).
 *                        global_id = the vehicle, data =
 *                        {"action":1|2|3,"node":<id>} JSON. FAIL-CLOSED:
 *                        unlike every other hook the request is DENIED
 *                        unless at least one verdict handler is registered
 *                        AND none denies -- registering a handler that
 *                        returns 0 is how a plugin opts the feature in
 *                        (together with [experimental] node_grab in
 *                        server.toml).
 *   "queueJoinRequest" / "queueAdmitRequest" - the join queue's two questions
 *                        (ABI 2.8), player_id = global_id = -1, data = JSON
 *                        naming accounts (a waiting player has no id). DENY
 *                        refuses the wait with `reason` / lets nobody in for
 *                        now; a Lua handler can also answer a place or an
 *                        account, a module uses queue_move / queue_admit.
 *
 * These NINE are cancellable: the decisions where the client genuinely
 * waits on the server before the action takes effect, plus the two
 * optimistic config mutations (edit/paint) that the server can roll back
 * to its cached truth. A client's own reset/delete stay observe-only
 * (no clean previous value to restore) - observe them with
 * register_vehicle_event / the "vehicleDeleted" builtin instead.
 *
 * data is NUL-terminated and valid only for the duration of the call. */
typedef int (*NodeCancellableEventCallback)(int player_id, int global_id, const char* data,
    char* reason, int reason_cap, void* user);

/* Timer callback (set_timeout / set_interval). timer_id is the id returned
 * by the registration call. Runs on the framework worker thread. */
typedef void (*NodeTimerCallback)(int timer_id, void* user);

/* Log sink (set_log_sink). Invoked for EVERY line the server logs:
 *   tag   - the console tag ("Core", "Net", "Join", ...), or "" for raw
 *           untagged lines (startup banner, --help text).
 *   level - NODE_LOG_INFO/WARN/ERROR/DEBUG, derived from the tag.
 *   msg   - the plain message text (ANSI colors already stripped), without
 *           the timestamp/tag prefix.
 * Return nonzero to SUPPRESS the line (it reaches neither the console nor
 * logs/server.log); return 0 to let the server print it normally.
 * THREADING: runs inline on whatever thread produced the line, under the
 * server's log-hook lock - keep it fast, never block. Log lines produced
 * from inside the sink bypass the sink (no recursion). */
typedef int (*NodeLogSink)(const char* tag, int level, const char* msg, void* user);

/* Relay-visibility filter (register_relay_filter). Consulted for every
 * broadcast/relayed packet, once per (sender, recipient) delivery, and for
 * every vehicle bundle of a join replay / resync. Return 0 to let the
 * recipient see the packet, nonzero to HIDE it (one hiding filter wins,
 * like a cancellable veto). Arguments:
 *   from_player_id - the sending player, or -1 for a server-originated
 *                    broadcast (server spawn, seat transaction, ...);
 *   to_player_id   - the candidate recipient;
 *   category, subtype - the raw wire packet identity bytes;
 *   global_id      - the vehicle the packet is about, or -1 when the packet
 *                    carries no vehicle (session/event traffic).
 * THREADING: runs inline on whatever NETWORK thread is relaying (UDP
 * receive loop, per-client TCP threads, the framework worker) - it must be
 * fast, thread-safe and must never block. It can run under internal server
 * locks, so treat it as a PURE function of module data: never call
 * state-mutating NodeApi functions (seat_player, spawn_vehicle, kick_player,
 * ...) from inside the filter. Verdicts are CACHED per
 * (from, to, category, subtype, global_id); call invalidate_relay_cache()
 * whenever the data your filter reads changed (e.g. a player switched
 * dimension). The cache is also dropped automatically when filters are
 * (un)registered and when a player disconnects. */
typedef int (*NodeCanRelayCallback)(int from_player_id, int to_player_id,
    int category, int subtype, int global_id, void* user);

/* Background job (submit_job). `work` runs on a background pool thread -
 * it must NOT call into Lua and should not touch framework state that is
 * documented as worker-thread-only; every NodeApi function is safe. Its
 * return value is handed unchanged to `done`, which runs on the framework
 * worker thread (the same thread as every other callback). */
typedef void* (*NodeJobWorkFn)(void* user);
typedef void (*NodeJobDoneFn)(void* result, void* user);

/* HTTP response callback (http_request). Runs on the framework worker
 * thread. status is the HTTP status code, or -1 on a transport error (then
 * body carries the error text). body is NOT NUL-safe binary data of
 * body_len bytes (also NUL-terminated for convenience); headers_json is a
 * JSON object of response headers (lower-cased names). All pointers are
 * valid only for the duration of the call. */
typedef void (*NodeHttpCallback)(int status, const char* body, int body_len,
    const char* headers_json, void* user);

/* Inter-resource bus callback (register_resource_event). Runs on the
 * framework worker thread. source is the emitting resource's name ("native"
 * for a C module emit); data is the opaque payload string. */
typedef void (*NodeResourceEventCallback)(const char* source, const char* data, void* user);

/* One answer to call_resource_event: the answering resource and the value its
 * handler returned, as JSON. Runs on the framework worker thread before
 * call_resource_event returns; both strings are valid only during the call. */
typedef void (*NodeBusAnswerCallback)(const char* source, const char* value_json, void* user);

/* Module-channel callback (register_module_channel, wire v12): one inbound
 * client payload on the subscribed u32 channel id. data is ARBITRARY BINARY
 * bytes of len (also NUL-terminated one past the end for convenience) and is
 * valid only for the duration of the call. Runs on the framework worker
 * thread. Client->server module data is server-terminal: it reaches these
 * subscribers and is never auto-relayed -- relay explicitly via send_module.
 * Channel ids are a module-chosen namespace (coordinate your own ids). */
typedef void (*NodeModuleChannelCallback)(int player_id, uint32_t channel,
    const void* data, int len, void* user);

/* Kinematics of one vehicle from its last State/Pos snapshot, filled by
 * get_vehicle_transform. Vectors are x,y,z; rot is the orientation
 * quaternion x,y,z,w. Velocities are m/s and rad/s, already scaled by the
 * sender's sim speed (zero while its physics is paused). */
typedef struct NodeVehicleTransform {
    float pos[3]; /* world position, meters */
    float rot[4]; /* orientation quaternion x,y,z,w */
    float vel[3]; /* linear velocity, m/s */
    float ang_vel[3]; /* angular velocity, rad/s */
    uint32_t tim_ms; /* sender vehicle-VM timer, milliseconds */
    uint16_t ping_ms; /* sender ping + frame dt, milliseconds */
    uint16_t seq; /* per-vehicle send counter, wraps at 65536 */
    int is_transitioning; /* 1 while the sender's sim speed is ramping */
    int paused; /* 1 while the sender's physics is paused */
} NodeVehicleTransform;

/* Driving controls of one vehicle from its last State/Pos snapshot, filled
 * by get_vehicle_controls. steering is -1..1, the pedals are 0..1 (de-
 * quantized from the wire bytes). gear is the value BeamNG round-trips:
 * a decimal number ("3", "-1") for manual/sequential boxes, a letter form
 * ("P"/"R"/"N"/"D"/"S"/"2"/"1"/"M3") for automatic-style boxes, or "" when
 * the snapshot carried no gear data. */
typedef struct NodeVehicleControls {
    float steering; /* -1 (full left) .. 1 (full right) */
    float throttle; /* 0..1 */
    float brake; /* 0..1 */
    float clutch; /* 0..1 */
    float parking_brake; /* 0..1 (handbrake) */
    char gear[8]; /* NUL-terminated, see above */
} NodeVehicleControls;

/* Snapshot of one vehicle, filled by get_vehicle_info. */
typedef struct NodeVehicleInfo {
    int global_id; /* server-assigned id, unique within a server run */
    int spawner_id; /* player who spawned it, -1 once the spawner left */
    int driver_id; /* player in the seat, -1 when empty */
    int authority_id; /* player simulating physics, -1 when idle */
    int is_unicycle; /* 1 = walking avatar, 0 = normal vehicle */
    int spawner_online; /* 1 if spawner_id is still connected */
    double idle_seconds; /* seconds without a driver, 0.0 while driven */
    char owner_name[64]; /* name of the spawner at spawn time, NUL-terminated */
} NodeVehicleInfo;

/* A LANGUAGE HOST teaches the server a new kind of resource. Put
 *
 *     type = "js"
 *
 * in a resource.toml and the folder is handed to whichever host registered
 * that name; Lua is built in and needs no host. This is how a runtime for
 * another language plugs in without the server knowing anything about it:
 * the host embeds the interpreter, the server only decides who gets the
 * folder and forwards events.
 *
 * Register during node_plugin_init -- modules are loaded before resources, so
 * a host registered there is in place by the time resources are scanned.
 *
 * load() is called once per resource folder: `dir` is absolute, `entry` is the
 * manifest's server.main (already checked to stay inside the folder). Return 0
 * to accept. unload() is called on shutdown, and on reload for that name.
 *
 * WHICH THREAD. Not always the worker. The startup load() of every resource
 * runs on the MAIN thread, before the framework worker is started, so no
 * worker callback runs concurrently with it. A reload runs unload() and then
 * load() on the framework worker thread, in sequence with the dispatches (no
 * handler runs in between). At shutdown, unload() runs on the thread shutting
 * the server down, AFTER the worker has been joined -- no worker callback is
 * running or will run again -- in reverse load order. Those three paths never
 * overlap. unregister_language_host() is different: it calls unload() inline
 * on whatever thread calls it, without stopping the worker.
 *
 * Client files are packaged for a hosted resource exactly as for a Lua one --
 * what the client runs is BeamNG's Lua whatever language wrote the server half,
 * so client/ works the same and needs nothing from the host.
 *
 * WHO CLEANS UP (ABI 1.3). Registrations you make while the framework is
 * calling your load() are attributed to that resource, and the framework drops
 * them when the resource unloads -- event handlers, timers, relay filters, bus
 * subscriptions, module channels, the log sink. For registrations you make
 * LATER, from an event handler or your own thread, say who they are for with
 * set_resource_owner(name) and set_resource_owner(NULL) afterwards.
 *
 * The sweep runs AFTER your unload(), so cleaning up yourself still works and
 * simply leaves nothing to sweep. What it protects against is the case you
 * miss: a callback left in the dispatch tables is called later, and if the
 * memory behind it is gone by then -- a torn-down script context, an unloaded
 * module -- that is a crash, not a leak.
 *
 * WORK ALREADY IN FLIGHT IS NOT COVERED. submit_job and http_request are not
 * registrations; they are running. Their completion callbacks fire whether or
 * not the resource that started them is still loaded. Keep whatever they touch
 * alive until they land, or have them check on entry that their resource is
 * still there. */
typedef struct NodeLanguageHost {
    uint32_t struct_size; /* sizeof(NodeLanguageHost) as the MODULE built it */
    const char* type_name; /* matched against resource.toml `type` */
    int (*load)(const char* name, const char* dir, const char* entry, void* user);
    void (*unload)(const char* name, void* user);
    void* user;
} NodeLanguageHost;

typedef struct NodeApi {
    /* ------------------------------------------------------------------ */
    /* ABI contract                                                       */
    /* ------------------------------------------------------------------ */

    /* NODE_ABI_VERSION as the SERVER was built with. The loader has already
     * compared this against the module's node_plugin_abi() before calling
     * node_plugin_init, so a module that is running at all matches; the
     * field is here so a module can assert it too. */
    uint32_t abi_version;

    /* sizeof(NodeApi) as the SERVER built it. A module built against an
     * older minor sees a LARGER value than its own sizeof and can rely on
     * every field it knows about; a module built against a newer minor sees
     * a smaller one and must not touch anything past it. */
    uint32_t struct_size;

    /* ------------------------------------------------------------------ */
    /* Console and logging                                                */
    /* ------------------------------------------------------------------ */

    /* Log under the server's "Module" / "Warn" / "Error" console tag.
     * Inline, thread-safe. */
    void (*log_info)(const char* msg);
    void (*log_warn)(const char* msg);
    void (*log_error)(const char* msg);

    /* Log under an explicit server console tag. Known tags
     * (case-insensitive): Core, Net, Res, Mods, Module, Join, Leave, Kick,
     * Veh, Warn, Error, Debug. Unknown tags fall back to "Module". The line
     * uses the house format (HH:MM:SS Tag > msg) with the tag's standard
     * color. Inline. */
    void (*log_tag)(const char* tag, const char* msg);

    /* Print a line with the module's OWN tag label in a chosen color, in the
     * house line format. rgb is 0xRRGGBB (e.g. 0xFF8800); the server maps it
     * to the nearest ANSI-256 color, so the exact shade depends on the
     * terminal palette. Keep tags <= 6 characters to preserve the column
     * alignment of the console. The line is mirrored ANSI-stripped into
     * logs/server.log like every other line. Inline. */
    void (*log_custom)(const char* tag, uint32_t rgb, const char* msg);

    /* Write raw text bypassing the timestamp/tag format (like the startup
     * banner). The text may contain its own ANSI escapes; the copy written
     * to logs/server.log is ANSI-stripped. No newline handling beyond one
     * line per call. Inline. */
    void (*log_raw)(const char* text);

    /* Install a log sink that observes - and may suppress - every server log
     * line (see NodeLogSink). ONE sink slot exists per server: the last
     * registration wins, pass (NULL, NULL) to remove. After
     * set_log_sink(NULL, NULL) returns, the previous sink is guaranteed not
     * to be running and will not be called again (safe to free its user
     * data). The framework clears the sink automatically before modules are
     * unloaded. Inline. */
    void (*set_log_sink)(NodeLogSink sink, void* user);

    /* Set the console window/tab title. On Windows this calls
     * SetConsoleTitle; elsewhere it emits an OSC escape when the terminal
     * supports ANSI (silently does nothing otherwise). Inline. */
    void (*set_console_title)(const char* title);

    /* ------------------------------------------------------------------ */
    /* Players                                                            */
    /* ------------------------------------------------------------------ */

    /* Number of connected players. Inline. */
    int (*get_player_count)(void);

    /* Writes up to max player ids into ids; returns the number written.
     * Inline. */
    int (*get_players)(int* ids, int max);

    /* Writes the NUL-terminated player name into buf; returns the name
     * length (excluding the NUL), or -1 if the player does not exist or buf
     * is too small. Inline. */
    int (*get_player_name)(int id, char* buf, int buflen);

    /* Returns 1 if a player with this id is currently connected, else 0.
     * Inline. */
    int (*is_player_connected)(int id);

    /* Seconds since the last keepalive ping was received from this player,
     * or -1 if the player does not exist. Node does not measure round-trip
     * latency; this is a connection-freshness metric (0-1 for a healthy
     * client, growing when the client stalls). Inline. */
    int (*get_player_ping)(int id);

    /* globalId of the vehicle this player is currently DRIVING, or -1 if it
     * is not driving one (on foot, a passenger, or not found). For the
     * role-aware lookup that also finds a passenger seat, use
     * get_player_vehicle. Inline. */
    int (*get_player_current_vehicle)(int id);

    /* ------------------------------------------------------------------ */
    /* Roles, kicks and bans                                              */
    /* ------------------------------------------------------------------ */

    /* Kicks a player. Returns 0 on success, -1 if the player does not exist.
     * Inline (the disconnect itself completes asynchronously). */
    int (*kick_player)(int id, const char* reason);

    /* ------------------------------------------------------------------ */
    /* Vehicles: registry and records                                     */
    /* ------------------------------------------------------------------ */

    /* Number of vehicles currently registered in the world. Inline. */
    int (*get_vehicle_count)(void);

    /* Writes up to max vehicle globalIds into ids; returns the number
     * written. Inline. */
    int (*get_vehicles)(int* ids, int max);

    /* Fills out with a snapshot of the vehicle. Returns 0 on success, -1 if
     * the vehicle does not exist (out is left untouched). Inline. */
    int (*get_vehicle_info)(int global_id, NodeVehicleInfo* out);

    /* Writes up to max globalIds of vehicles this player spawned or is
     * driving into ids; returns the number written. Inline. */
    int (*get_player_vehicles)(int player_id, int* ids, int max);

    /* The single vehicle a player currently occupies in ANY role (wire v9).
     * Returns its globalId, or -1 when the player is on foot / not found.
     * When out_role is non-NULL it receives the seat role
     * (NODE_ROLE_NONE/DRIVER/ PASSENGER). Inline. */
    int (*get_player_vehicle)(int player_id, int* out_role);

    /* Occupants of a vehicle (wire v9): the driver first (role
     * NODE_ROLE_DRIVER) then each passenger (NODE_ROLE_PASSENGER). Writes up
     * to max player ids into ids and, when roles is non-NULL, the matching
     * role into roles[i]; returns the occupant count (0 if the vehicle does
     * not exist or is empty). Inline. */
    int (*get_vehicle_occupants)(int global_id, int* ids, int* roles, int max);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* The damage version last reported by the vehicle's sync authority (0 =
     * pristine; grows as the vehicle takes damage; resets on repair/
     * respawn). -1 if the vehicle does not exist. Pair with the
     * "vehicleDamageChanged" builtin to detect changes and offer a resync
     * (resync_vehicle). Inline. */
    int (*get_vehicle_damage_counter)(int global_id);

    /* Fills out with the vehicle's kinematics decoded from its last
     * State/Pos snapshot (the high-rate stream the sync authority sends over
     * UDP). Returns 0 on success, -1 if the vehicle does not exist or no
     * snapshot has arrived yet (out is left untouched). Inline. */
    int (*get_vehicle_transform)(int global_id, NodeVehicleTransform* out);

    /* Fills out with the driving controls folded into the same snapshot.
     * Returns 0 on success, -1 if the vehicle does not exist, no snapshot
     * has arrived yet, or the last snapshot carried no valid controls (the
     * sender was not the driver); out is left untouched on -1. Inline. */
    int (*get_vehicle_controls)(int global_id, NodeVehicleControls* out);

    /* Writes the vehicle's current electrics (the merged view of the
     * State/Electrics delta stream: lights, signals, gauges, ...) as a
     * NUL-terminated JSON object string into buf. Returns the string length
     * (excluding the NUL), or -1 if the vehicle does not exist, no electrics
     * arrived yet, or buf is too small (16 KB is comfortably enough in
     * practice). Inline. */
    int (*get_vehicle_electrics_json)(int global_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* Deletes a vehicle: broadcasts the removal to every client, removes it
     * from the registry and fires the "vehicleDeleted" builtin (delivered
     * asynchronously on the worker thread). Returns 0 on success, -1 if the
     * vehicle does not exist. Inline. */
    int (*delete_vehicle)(int global_id);

    /* Deletes every vehicle in the world; returns how many were deleted.
     * Inline. */
    int (*delete_all_vehicles)(void);

    /* Server-driven coupler/door actuation: opens (open = 1) or closes (open
     * = 0) the named advanced-coupler group (e.g. "doorFLCoupler",
     * "hood_latch_coupler") on EVERY client - including the driver - and
     * merges the resulting absolute state into the join-replay cache, so a
     * late joiner sees the door in its new state. Skips the
     * vehicleCouplerRequest permission builtin (the server never asks
     * itself). Returns 0 on success, -1 if the vehicle does not exist.
     * Inline. */
    int (*set_vehicle_coupler)(int global_id, const char* name, int open);

    /* Server-driven seating (Vehicle Authority V2). These move a player
     * between driver seats WITHOUT the cancellable
     * vehicleEnterRequest/vehicleExitRequest hooks (the server never asks
     * itself, like set_vehicle_coupler) but emit the same
     * Driver/Authority/SeatFree/PlayerVehicle broadcasts, so the target
     * client's camera/control follow exactly like an ordinary enter/exit and
     * observers fire as usual. All inline; the seat mutation itself
     * completes before the call returns, the client-side camera move follows
     * asynchronously over the network. seat_player: seats player_id in
     * global_id. seat_kind is NODE_SEAT_DRIVER or NODE_SEAT_PASSENGER. As a
     * DRIVER the player leaves any car it drives, its walking-avatar
     * unicycle is deleted, and a previous driver is put on foot. As a
     * PASSENGER it joins the occupant list (allowed even in a driverless
     * car; the wheel is untouched). Returns 0 on success, -1 on invalid args
     * or a unicycle target. */
    int (*seat_player)(int player_id, int global_id, int seat_kind);

    /* unseat_player: forces the player out of the car it occupies as DRIVER
     * or PASSENGER (on foot; the client spawns its unicycle as on a normal
     * exit). Returns 0 on success, -1 if the player is on foot (in no
     * vehicle). */
    int (*unseat_player)(int player_id);

    /* swap_players: atomically exchanges the two players' seats -- role AND
     * vehicle -- in one seat transaction, implementing the full driver/
     * passenger/pedestrian matrix (driver<->driver swaps cars; driver<->
     * passenger of one car exchanges roles; a seat<->pedestrian swap rides
     * one and walks the other, ...). Returns 0 on success, -1 when player_a
     * == player_b, both are on foot, or they share the same passenger seat. */
    int (*swap_players)(int player_a, int player_b);

    /* Re-sends a vehicle's full cached authoritative state (spawn/config,
     * position, electrics, doors/couplers, powertrain, controller state,
     * driver/authority; the damage blob rides along) to one player, or to
     * every synced player when player_id is -1. Pass global_id -1 to resync
     * ALL vehicles. Returns the number of vehicle bundles sent. The client
     * re-applies the bundle like a join replay ("any desync can always be
     * made a sync"). Inline. */
    int (*resync_vehicle)(int global_id, int player_id);

    /* Re-sends EVERY vehicle's full cached state to one player; equivalent
     * to resync_vehicle(-1, player_id). Returns the number of vehicle
     * bundles sent. Inline. */
    int (*resync_player)(int player_id);

    /* ------------------------------------------------------------------ */
    /* Vehicle tags                                                       */
    /* ------------------------------------------------------------------ */

    /* Stores/overwrites one tag. Returns 0 on success, -1 when the vehicle
     * does not exist, key/value is NULL or empty, or a limit is broken
     * (empty values are not legal tags -- remove with remove_vehicle_tag). */
    int (*set_vehicle_tag)(int global_id, const char* key, const char* value);

    /* Writes the NUL-terminated tag value into buf; returns the value length
     * (excluding the NUL), or -1 if the vehicle or the tag does not exist or
     * buf is too small (257 bytes always fits). */
    int (*get_vehicle_tag)(int global_id, const char* key, char* buf, int buflen);

    /* Removes one tag. Returns 0 when the tag existed and was removed, -1
     * otherwise. */
    int (*remove_vehicle_tag)(int global_id, const char* key);

    /* Writes the vehicle's whole tag set as a NUL-terminated JSON object
     * string ({"key":"value",...}, keys sorted) into buf. Returns the string
     * length (excluding the NUL), or -1 if the vehicle does not exist or buf
     * is too small (16 KB always fits the 32x(64+256) worst case). */
    int (*get_vehicle_tags_json)(int global_id, char* buf, int buflen);

    /* Reverse lookup: writes up to max globalIds of vehicles carrying the
     * tag `key` into out_ids -- any value when value_or_null is NULL,
     * exactly value_or_null otherwise. Returns the number written, sorted
     * ascending by globalId. For a multi-pair AND match call this once and
     * intersect, or use the Lua node.getVehiclesByTags. */
    int (*get_vehicles_by_tag)(const char* key, const char* value_or_null, int* out_ids, int max);

    /* ------------------------------------------------------------------ */
    /* Events: subscribe and emit                                         */
    /* ------------------------------------------------------------------ */

    /* Sends one custom EVENT frame (name + data) to one client over TCP.
     * Returns 0 on success, -1 on failure (unknown player, or player not
     * joined yet). Inline. */
    int (*emit_client)(int id, const char* event, const char* data);

    /* Sends one custom EVENT frame to all fully-connected clients. Inline. */
    void (*emit_all)(const char* event, const char* data);

    /* Subscribes to a custom client event by name. The callback runs on the
     * framework worker thread. Inline. */
    void (*register_client_event)(const char* event, NodeClientEventCallback cb, void* user);

    /* Removes a client-event subscription previously made with
     * register_client_event. Both cb AND user must match the registration. A
     * dispatch already in flight on the worker thread may still deliver one
     * final call. Inline. */
    void (*unregister_client_event)(const char* event, NodeClientEventCallback cb, void* user);

    /* Subscribes an OBSERVER to a builtin event by name (see
     * NodeBuiltinEventCallback). This also observes the cancellable request
     * builtins without being able to deny them. Unknown builtin names are
     * rejected with a console error. Inline. */
    void (*register_builtin_event)(const char* name, NodeBuiltinEventCallback cb, void* user);

    /* Removes a builtin-event subscription previously made with
     * register_builtin_event. Both cb AND user must match. Same in-flight
     * caveat as unregister_client_event. Inline. */
    void (*unregister_builtin_event)(const char* name, NodeBuiltinEventCallback cb, void* user);

    /* Subscribes a VERDICT callback to one of the cancellable request
     * builtins (see NodeCancellableEventCallback for the names and
     * payloads). The callback runs on the framework worker thread and
     * returns nonzero to deny the request. Unknown or non-cancellable names
     * are rejected with a console error. Inline. */
    void (*register_cancellable_event)(const char* name, NodeCancellableEventCallback cb, void* user);

    /* Removes a verdict subscription previously made with
     * register_cancellable_event. Both cb AND user must match. A dispatch
     * already in flight on the worker thread may still deliver one final
     * call. Inline. */
    void (*unregister_cancellable_event)(const char* name, NodeCancellableEventCallback cb, void* user);

    /* Subscribes an OBSERVER to a notification event by name (see
     * NodeVehicleEventCallback: "vehicleEdited", "vehicleReset",
     * "vehiclePainted", "playerSeatChanged", "vehicleCouplerChanged",
     * "vehicleControllerChanged", "playerVerifyReported" -- the server
     * judged a report the player's launcher sent; global_id is -1 there --
     * "playerLagging" / "playerRecovered" (player_id, -1, silentSec),
     * "vehicleDeleted" (player_id, global_id, cause), "playerLeft"
     * (player_id, -1, reason; server 1.6.0 -- the last two also keep their
     * one-argument register_builtin_event form) and "resourceUnload" -- (-1,
     * -1, reason) right before the registering resource's state is dropped,
     * reason "reload" or "shutdown"; dispatched to that resource's own
     * handlers only, so only a hosted resource ever receives it, never a
     * native module). The callback runs on the framework worker thread and
     * carries (player_id, global_id, data); its return value is ignored -
     * the action already happened. Unknown names are rejected with a console
     * error. Inline. */
    void (*register_vehicle_event)(const char* name, NodeVehicleEventCallback cb, void* user);

    /* Removes a vehicle-notification subscription previously made with
     * register_vehicle_event. Both cb AND user must match. A dispatch
     * already in flight on the worker thread may still deliver one final
     * call. Inline. */
    void (*unregister_vehicle_event)(const char* name, NodeVehicleEventCallback cb, void* user);

    /* ------------------------------------------------------------------ */
    /* Timers and scheduling                                              */
    /* ------------------------------------------------------------------ */

    /* Runs cb once after ms milliseconds, on the framework worker thread.
     * Returns a positive timer id, or -1 if cb is NULL. Pending timers never
     * fire after the framework shuts down. Registration is inline. */
    int (*set_timeout)(uint32_t ms, NodeTimerCallback cb, void* user);

    /* Runs cb every ms milliseconds until cleared. Same semantics as
     * set_timeout otherwise. Returns a positive timer id, or -1. */
    int (*set_interval)(uint32_t ms, NodeTimerCallback cb, void* user);

    /* Cancels a timer created by set_timeout/set_interval. Safe to call with
     * an already-fired or unknown id (no-op). A callback that is already
     * mid-execution on the worker thread finishes, but an interval will not
     * fire again. Inline. */
    void (*clear_timer)(int timer_id);

    /* ------------------------------------------------------------------ */
    /* Server: info, settings, time and metrics                           */
    /* ------------------------------------------------------------------ */

    /* The string getters write a NUL-terminated value into buf and return
     * its length (excluding the NUL), or -1 if buf is too small (128 bytes
     * is always enough for name/version; maps can be longer, 256 is safe).
     * All inline. */
    int (*get_server_name)(char* buf, int buflen); /* [server] name */
    int (*get_map)(char* buf, int buflen); /* [server] map */
    int (*get_server_version)(char* buf, int buflen); /* e.g. "3.9.3" */
    int (*get_port)(void); /* TCP+UDP listening port */
    int (*get_max_players)(void); /* [server] max_players */
    int (*get_max_cars)(void); /* [server] cars_per_player */

    /* ------------------------------------------------------------------ */
    /* Players                                                            */
    /* ------------------------------------------------------------------ */

    /* Writes the player's remote IP (IPv4 dotted / IPv6, NUL-terminated;
     * v4-mapped v6 is normalized to plain v4) into buf; returns its length,
     * or -1 if the player does not exist or buf is too small (64 bytes is
     * always enough). Inline. */
    int (*get_player_ip)(int id, char* buf, int buflen);

    /* The player's remote TCP port, or -1 if the player does not exist.
     * Inline. */
    int (*get_player_port)(int id);

    /* World position of the player: the vehicle it occupies (any role), or
     * its walking-avatar unicycle when on foot. Writes x,y,z into out_pos
     * and returns 0, or -1 when the player does not exist or no position
     * snapshot arrived yet. Inline. */
    int (*get_player_position)(int id, float out_pos[3]);

    /* ------------------------------------------------------------------ */
    /* Roles, kicks and bans                                              */
    /* ------------------------------------------------------------------ */

    /* Assigns/reads a free-form role string on a CONNECTED player. Roles are
     * per-session (keyed by player id, cleared on disconnect) - persist
     * anything long-lived yourself via the storage API keyed by IP. set: 0
     * on success, -1 unknown player (empty role clears). get: role length,
     * -1 unknown player / no role / buf too small. */
    int (*set_player_role)(int id, const char* role);
    int (*get_player_role)(int id, char* buf, int buflen);

    /* Bans the player's IP (persisted to bans.json next to server.toml) and
     * kicks it with `reason`. Returns 0, or -1 if the player is unknown.
     * NOTE: identity is the IP only (VPN-evadable) - a stronger per-install
     * identity needs a wire change and is deferred. */
    int (*ban_player)(int id, const char* reason);

    /* Bans/unbans one IP string (normalized like get_player_ip). ban_ip
     * affects FUTURE connects only (existing sessions stay). Returns 0 on
     * success; unban_ip returns -1 when the IP was not banned. */
    int (*ban_ip)(const char* ip, const char* reason);
    int (*unban_ip)(const char* ip);

    /* 1 iff the IP is currently banned. Inline. */
    int (*is_ip_banned)(const char* ip);

    /* Writes the whole ban list as a NUL-terminated JSON array
     * [{"ip","account","reason","at","name","until"},...] into buf; returns
     * its length or -1 when buf is too small. An IP ban has "ip" set and
     * "account" null; an account ban (v17) has "account" set (the
     * directory's id) and "ip" empty. "until" is the unix second a timed ban
     * ends (ABI 2.5), null for a ban for good; bans whose time is up are
     * dropped before the list is written. */
    int (*get_bans_json)(char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* The vehicle's current spawn/edit config JSON (what a joiner receives
     * inside Vehicle::Spawn). */
    int (*get_vehicle_config_json)(int global_id, char* buf, int buflen);

    /* Merged coupler/door state as a JSON array of coupler entries (the same
     * schema the clients exchange; one entry per known coupler). */
    int (*get_vehicle_couplers_json)(int global_id, char* buf, int buflen);

    /* Merged powertrain / engine device state objects. */
    int (*get_vehicle_powertrain_json)(int global_id, char* buf, int buflen);
    int (*get_vehicle_engine_json)(int global_id, char* buf, int buflen);

    /* Merged controller-call map: {"controller|function": {call json}}. */
    int (*get_vehicle_controllers_json)(int global_id, char* buf, int buflen);

    /* Copies the vehicle's baked-damage blob into buf and returns its size
     * (0 = pristine). When out_counter is non-NULL it receives the damage
     * counter the blob was made at. buf may be NULL to query the size
     * (buflen ignored then); with a non-NULL buf that is too small, -1.
     * Returns -1 when the vehicle does not exist. */
    int (*get_vehicle_damage_blob)(int global_id, uint8_t* buf, int buflen, uint32_t* out_counter);

    /* One-call world snapshot: {"time":unix,"players":[{id,name,ip,vehicle,
     * role,pos:[x,y,z]|null}],"vehicles":[{id,spawner,driver,authority,
     * unicycle,pos,rot,vel}]} - the bulk position read for radars/maps.
     * Returns the JSON length or -1 when buf is too small. */
    int (*get_world_snapshot_json)(char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* Spawns a vehicle FROM THE SERVER: registers it (spawner -1 =
     * ownerless), broadcasts the Vehicle::Spawn to every client and assigns
     * the best available sync authority (nobody takes the driver seat; use
     * seat_player to seat someone). config_json must be a JSON object in the
     * same schema clients send in SpawnReq (jbm, vcf, pos, rot, ...);
     * unicycle configs are rejected. owner_name is the display label (NULL
     * -> "server"). Returns the new globalId, or -1 on invalid config.
     * Inline; fires the "vehicleSpawned" builtin asynchronously. */
    int (*spawn_vehicle)(const char* config_json, const char* owner_name);

    /* ------------------------------------------------------------------ */
    /* Vehicle tags                                                       */
    /* ------------------------------------------------------------------ */

    /* Writes up to max globalIds of vehicles whose tag set contains EVERY
     * key/value pair of pairs_json (a JSON object {"key":"value",...});
     * returns the number written, ascending. An empty/invalid object matches
     * nothing. The C mirror of Lua node.getVehiclesByTags. */
    int (*get_vehicles_by_tags)(const char* pairs_json, int* out_ids, int max);

    /* ------------------------------------------------------------------ */
    /* Visibility: relay filter and groups                                */
    /* ------------------------------------------------------------------ */

    void (*register_relay_filter)(NodeCanRelayCallback cb, void* user);
    void (*unregister_relay_filter)(NodeCanRelayCallback cb, void* user);

    /* Drops every cached relay verdict so filters are re-consulted. Call
     * after the data your filter reads changed. Inline, thread-safe. */
    void (*invalidate_relay_cache)(void);

    /* ------------------------------------------------------------------ */
    /* Persistent storage                                                 */
    /* ------------------------------------------------------------------ */

    /* Values are JSON: set takes any JSON value text (a bare string that is
     * not valid JSON is stored as a string), get returns the JSON-encoded
     * value. Stores are named files under storage/ next to server.toml (Lua
     * resources use their resource name; pick any [A-Za-z0-9_.-]+ name,
     * sharing one store with a Lua resource is allowed). Data survives
     * server restarts; every mutation is flushed atomically. set: 0 ok / -1
     * bad store or key. get: value length / -1 missing or buf too small.
     * delete: 0 removed / -1 not present. All inline. Pass store = NULL for
     * the store belonging to the resource making the call, which is what the
     * scripting form means; name a store explicitly to reach one of your
     * own. A NULL store with no resource in scope -- a module calling from a
     * thread of its own -- is refused rather than written somewhere
     * arbitrary. */
    int (*storage_set)(const char* store, const char* key, const char* value_json);
    int (*storage_get)(const char* store, const char* key, char* buf, int buflen);
    int (*storage_delete)(const char* store, const char* key);

    /* ------------------------------------------------------------------ */
    /* Server: info, settings, time and metrics                           */
    /* ------------------------------------------------------------------ */

    /* Seconds since the server started (monotonic, fractional). Inline. */
    double (*get_server_uptime)(void);

    /* Current unix time in seconds. Inline. */
    int64_t (*get_unix_time)(void);

    /* Changes the live [server] limits (enforced on the next check; no
     * client is kicked retroactively) / the server name label. Values < 0
     * are rejected (-1). Inline, thread-safe. */
    int (*set_max_cars)(int n);
    int (*set_max_players)(int n);
    int (*set_server_name)(const char* name);

    /* ------------------------------------------------------------------ */
    /* Coroutines and background jobs                                     */
    /* ------------------------------------------------------------------ */

    /* Runs `work` on a background pool thread and hands its return value to
     * `done` on the framework worker thread (see NodeJobWorkFn). Returns 0,
     * or -1 when work is NULL. done may be NULL (fire-and-forget). */
    int (*submit_job)(NodeJobWorkFn work, NodeJobDoneFn done, void* user);

    /* ------------------------------------------------------------------ */
    /* HTTP                                                               */
    /* ------------------------------------------------------------------ */

    /* Asynchronous HTTP(S) request from a background pool thread; cb runs on
     * the framework worker thread (see NodeHttpCallback). method is a token
     * of uppercase letters A-Z only, at most 16 ("GET", "POST", "PUT",
     * "PATCH", "DELETE", "HEAD", ...; anything else is refused with status
     * -1 "invalid HTTP method" in the callback); headers_json an optional
     * JSON object of extra request headers (NULL = none); body/body_len the
     * optional request body (NULL/0 = none). Follows up to 5 redirects, ~15
     * s total deadline, 8 MB response cap. TLS verifies the peer: the chain
     * against the machine's roots plus the shipped cacert.pem (or the bundle
     * in [http] ca_file) and the name against the host; a failure is
     * reported like any transport error (status -1, the TLS error text as
     * the body). [http] insecure turns verification off. Returns 0, or -1 on
     * invalid arguments. */
    int (*http_request)(const char* method, const char* url, const char* headers_json, const void* body, int body_len, NodeHttpCallback cb, void* user);

    /* ------------------------------------------------------------------ */
    /* Inter-resource bus                                                 */
    /* ------------------------------------------------------------------ */

    /* Publishes one named event with an opaque data string to every bus
     * subscriber (Lua node.onResource + C register_resource_event), on the
     * framework worker thread. The C emitter appears as source "native". */
    void (*emit_resource_event)(const char* name, const char* data);
    void (*register_resource_event)(const char* name, NodeResourceEventCallback cb, void* user);
    void (*unregister_resource_event)(const char* name, NodeResourceEventCallback cb, void* user);

    /* ------------------------------------------------------------------ */
    /* Crypto and JSON                                                    */
    /* ------------------------------------------------------------------ */

    /* SHA-256 / HMAC-SHA256 as a lower-case hex string (64 chars + NUL)
     * written into out_hex65. Inline. */
    void (*sha256_hex)(const void* data, int len, char out_hex65[65]);
    void (*hmac_sha256_hex)(const void* key, int key_len, const void* data, int data_len, char out_hex65[65]);

    /* Fills out with n cryptographically random bytes (OpenSSL RAND).
     * Returns 0, or -1 when the generator failed / n is out of 1..65536. */
    int (*random_bytes)(void* out, int n);

    /* ------------------------------------------------------------------ */
    /* Server: info, settings, time and metrics                           */
    /* ------------------------------------------------------------------ */

    /* Writes a JSON object of live server metrics: players, vehicles,
     * uptime, unix time, TCP/UDP packet counters, flood kicks, connection-
     * limiter stats, plugin queue depth, timer/async task counts, and since
     * ABI 2.3 "platform" (get_platform_json) and "memory" (get_memory_json)
     * so an exporter gets everything in one call. Returns the JSON length or
     * -1 when buf is too small (4 KB is plenty). */
    int (*get_metrics_json)(char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Module channel (binary)                                            */
    /* ------------------------------------------------------------------ */

    /* Sends one payload on `channel`: targeted to player_id, or to EVERY
     * synced client when player_id is -1 (the broadcast respects the
     * relayRequest relay filter). Returns 0 on success, -1 when the target
     * player is unknown/not joined or data is NULL with len > 0. Inline. */
    int (*send_module)(int player_id, uint32_t channel, const void* data, int len);

    /* Subscribes to inbound client payloads on one channel id (see
     * NodeModuleChannelCallback). Inline. */
    void (*register_module_channel)(uint32_t channel, NodeModuleChannelCallback cb, void* user);

    /* Removes a module-channel subscription. Both cb AND user must match. A
     * dispatch already in flight on the worker thread may still deliver one
     * final call. Inline. */
    void (*unregister_module_channel)(uint32_t channel, NodeModuleChannelCallback cb, void* user);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* Sets the vehicle's lock: mode is NODE_LOCK_ above, whitelist is an
     * array of `count` player ids that are ALWAYS allowed (NULL/0 = empty;
     * <= 128 entries since ABI 2.2, an over-limit set is rejected;
     * account-keyed entries need set_vehicle_lock_named). "unlocked" clears
     * the whitelist. Broadcast to every client as Vehicle::Lock, replayed to
     * late joiners, fires the "vehicleLockChanged" builtin. A lock set here
     * is PLUGIN-OWNED: the spawner's own Session::Policy (wire v21) no
     * longer touches this vehicle until release_vehicle_lock. Returns 0 on
     * success, -1 on an unknown vehicle / bad mode / over-limit whitelist.
     * Inline. */
    int (*set_vehicle_lock)(int global_id, int mode, const int* whitelist, int count);

    /* Reads the vehicle's lock: returns the NODE_LOCK_ mode (or -1 for an
     * unknown vehicle) and, when out_whitelist is non-NULL, writes up to max
     * whitelisted player ids into it, storing the count in out_count
     * (out_count may be NULL). Inline. */
    int (*get_vehicle_lock)(int global_id, int* out_whitelist, int max, int* out_count);

    /* Has the vehicle's SYNC AUTHORITY execute one controller call
     * (call_json = the Vehicle::Trigger schema: {"controllerName":...,
     * "functionName":..., ...}). Skips the vehicleTriggerRequest hook (the
     * server never asks itself); never cached -- the resulting state changes
     * ride the normal sync channels. Returns 0 on success, -1 when the
     * vehicle is gone, idle (no authority) or call_json is not a JSON
     * object. Inline. */
    int (*trigger_vehicle)(int global_id, const char* call_json);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* The vehicle's current config marker (FNV-1a-32 over the cached config
     * JSON bytes exactly as Spawn/Resync send them) -- what clients echo in
     * their periodic Vehicle::ConfigHash reports. Writes it into out_hash
     * and returns 0, or -1 for an unknown vehicle. Inline. */
    int (*get_vehicle_config_hash)(int global_id, uint32_t* out_hash);

    /* ------------------------------------------------------------------ */
    /* Server: info, settings, time and metrics                           */
    /* ------------------------------------------------------------------ */

    /* 1 iff the experimental node grabber is enabled in server.toml
     * ([experimental] node_grab). Even when enabled, every request still
     * needs an explicit ALLOW from the fail-closed vehicleNodeGrabRequest
     * hook. Inline. */
    int (*is_node_grab_enabled)(void);

    /* ------------------------------------------------------------------ */
    /* Language hosts and registration ownership                          */
    /* ------------------------------------------------------------------ */

    /* Register a runtime for a resource `type` (see NodeLanguageHost). Call
     * from node_plugin_init: modules load before resources, so a host
     * registered there is in place when resources are scanned. Returns 0, or
     * -1 when host/type_name is NULL, struct_size does not match, or that
     * type is already taken (including the built-in "lua"). */
    int (*register_language_host)(const NodeLanguageHost* host);

    /* Drop a previously registered host. Its loaded resources are unloaded
     * first, so unload() runs for each while the module is still there. What
     * those resources registered is NOT removed for you -- see
     * NodeLanguageHost: undo it in unload(), or the framework will keep
     * calling into a module that is no longer loaded. Returns 0, or -1 when
     * no host owns that type. */
    int (*unregister_language_host)(const char* type_name);

    /* ------------------------------------------------------------------ */
    /* Events: subscribe and emit                                         */
    /* ------------------------------------------------------------------ */

    /* Like emit_client, but the payload is length-delimited and may contain
     * NUL bytes. Event data is opaque on the wire, and Lua resources have
     * always been able to send binary through it; emit_client cannot,
     * because its payload is a C string and stops at the first NUL. Use this
     * one for anything that is not guaranteed text. Returns 0 on success, -1
     * if the player does not exist. */
    int (*emit_client_bytes)(int id, const char* event, const void* data, int len);

    /* Like emit_all, but the payload is length-delimited and may contain NUL
     * bytes. See emit_client_bytes. */
    void (*emit_all_bytes)(const char* event, const void* data, int len);

    /* ------------------------------------------------------------------ */
    /* Language hosts and registration ownership                          */
    /* ------------------------------------------------------------------ */

    /* Attributes everything registered from THIS THREAD from now on to
     * `resource_name`, so the framework can drop it when that resource
     * unloads. Pass NULL to go back to registering on your own behalf, which
     * is the default and means the registration lives as long as the module
     * does. Registrations a host makes while the framework is calling its
     * load() are attributed automatically; this is for the ones made later,
     * from an event handler or the host's own thread. It applies per thread
     * and until changed, so set it, register, and set NULL again. */
    void (*set_resource_owner)(const char* resource_name);

    /* ------------------------------------------------------------------ */
    /* Vehicles: registry and records                                     */
    /* ------------------------------------------------------------------ */

    /* The complete vehicle record as a JSON object: id, spawner, driver,
     * authority, isUnicycle, name, idleSeconds, passengers, spawnerOnline,
     * lockMode, partsHash (the build identity, ABI 2.2), lockWhitelist,
     * configHash, damageCounter, damageBlobCounter, damageBlobSize, tags.
     * get_vehicle_info fills a fixed struct and cannot carry the
     * variable-length parts (passengers, the lock whitelist, tags), which is
     * why this exists alongside it. Returns the length written, -1 if there
     * is no such vehicle. Ask with buf = NULL for the size. */
    int (*get_vehicle_json)(int global_id, char* buf, int buflen);

    /* Every vehicle as a JSON array of the records get_vehicle_json returns.
     * Returns the length written. Ask with buf = NULL for the size. */
    int (*get_vehicles_json)(char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Players                                                            */
    /* ------------------------------------------------------------------ */

    /* Connected players as a JSON array of { id, name }. An array of records
     * rather than a map of id to name: a map keyed by a number has no
     * faithful JSON form, so it could not be the same shape in two
     * languages. Returns the length written. Ask with buf = NULL for the
     * size. */
    int (*get_players_json)(char* buf, int buflen);

    /* A player's network endpoint as JSON { ip, port }. get_player_ip and
     * get_player_port answer the same two things separately, which is fine
     * for C and awkward for a scripting caller. Returns the length written,
     * -1 if there is no such player. Ask with buf = NULL for the size. */
    int (*get_player_endpoint_json)(int id, char* buf, int buflen);

    /* A player's last known world position as JSON { x, y, z }, taken from
     * the vehicle they occupy. Returns the length written, -1 when there is
     * no such player or no position has arrived yet. Ask with buf = NULL for
     * the size. */
    int (*get_player_position_json)(int id, char* buf, int buflen);

    /* What the player is in, as JSON { gid, role } with role "driver" or
     * "passenger". get_player_current_vehicle answers the driver case only.
     * Returns the length written, -1 if the player is on foot or does not
     * exist. Ask with buf = NULL for the size. */
    int (*get_player_vehicle_json)(int id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* Damage telemetry as JSON { counter, blobCounter, size }: the
     * authority-reported damage version, the version the stored blob was
     * made at, and the blob size (0 = pristine). Returns the length written,
     * -1 if there is no such vehicle. Ask with buf = NULL for the size. */
    int (*get_vehicle_damage_json)(int global_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: registry and records                                     */
    /* ------------------------------------------------------------------ */

    /* Who is in a vehicle, driver first, as a JSON array of { playerId, role
     * } with role "driver" or "passenger". get_vehicle_occupants returns the
     * same information as two parallel arrays, which is harder to consume
     * from a scripting language. Returns the length written, -1 if there is
     * no such vehicle. Ask with buf = NULL for the size. */
    int (*get_vehicle_occupants_json)(int global_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Resources: lifecycle                                               */
    /* ------------------------------------------------------------------ */

    /* Queues a reload: the resource is handed back to its language host and
     * loaded again from the same folder. Uniform across runtimes, Lua
     * included, because loading is -- the host is not told it is a reload.
     * Everything the resource registered is dropped first, so the new
     * instance never shares the dispatch tables with the old one. QUEUED,
     * not immediate: a reload destroys the resource's registrations, and the
     * usual place to ask for one is inside a handler, which IS one of those
     * registrations, running. It therefore happens on the worker once the
     * current dispatch finishes. Returns 0 when the request was accepted, -1
     * when no resource by that name is loaded. A host that then refuses the
     * load leaves the resource unloaded rather than half-loaded, and says so
     * in the log. */
    int (*reload_resource)(const char* name);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* Kinematics from the last position snapshot as JSON: pos, rot (a
     * quaternion), vel, angVel, time, ping, seq, paused, transitioning,
     * teleport, velocityStep, hasControls, ageSeconds. Velocities are m/s,
     * angular velocity rad/s, time and ping the sender's own timers in
     * seconds, ageSeconds since the server accepted the snapshot. teleport /
     * velocityStep are the sender's step flags for that snapshot (receivers
     * snap instead of blending); hasControls says whether the control bytes
     * were meaningful (the sender was the driver). get_vehicle_transform
     * fills a fixed struct with the same values. Returns the length written,
     * -1 if there is no such vehicle or no snapshot arrived. Ask with buf =
     * NULL for the size. */
    int (*get_vehicle_transform_json)(int global_id, char* buf, int buflen);

    /* Driving controls from the same snapshot as JSON: steering, throttle,
     * brake, clutch, parkingBrake, and gear when the sender reported one --
     * a number for manual boxes, a letter for the automatic ones ("P", "R",
     * "N", "D", "S", "2", "1", "M3"). Returns the length written, -1 until a
     * snapshot arrives whose controls are meaningful, which is only when the
     * sender was the driver. Ask with buf = NULL for the size. */
    int (*get_vehicle_controls_json)(int global_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* The vehicle lock as JSON { mode, whitelist, owner }: mode is
     * "unlocked", "locked" or "driver-only", whitelist the player ids
     * allowed in regardless, owner "plugin" when a plugin set the lock (the
     * spawner's Session::Policy leaves it alone) or "policy" (the spawner's
     * own preference, or no lock at all). get_vehicle_lock answers mode and
     * whitelist through an out array and an out count. Returns the length
     * written, -1 if there is no such vehicle. Ask with buf = NULL for the
     * size. */
    int (*get_vehicle_lock_json)(int global_id, char* buf, int buflen);

    /* Sets the lock by NAMED mode: "unlocked", "locked" or "driver-only"
     * (case-insensitive; "lockedAll" and "driverOnly" are accepted too).
     * whitelist_json is an optional JSON array of the player ids allowed in
     * regardless -- NULL for none. set_vehicle_lock takes the wire's integer
     * and a C array instead, which is the transport's business rather than
     * the caller's. Returns 0, or -1 for an unknown mode, an unparseable
     * whitelist or an unknown vehicle. */
    int (*set_vehicle_lock_named)(int global_id, const char* mode, const char* whitelist_json);

    /* Seats a player by NAMED role: "driver" (the default when role is NULL
     * or empty) or "passenger". seat_player takes the wire's integer
     * instead, which is the transport's business rather than the caller's.
     * Returns 0 on success, -1 for an unknown role or when the seat cannot
     * be taken. */
    int (*seat_player_role)(int player_id, int global_id, const char* role);

    /* ------------------------------------------------------------------ */
    /* Visibility: relay filter and groups                                */
    /* ------------------------------------------------------------------ */

    /* Puts a player in a visibility group. Two things are visible to each
     * other only when their group numbers match, and 0 -- the default -- is
     * the world everyone shares. That is the whole rule, and it covers
     * parallel worlds, instanced races, private lobbies and spectator
     * islands. Prefer this over a relay filter for anything of the kind. A
     * filter lives in a script, scripts run on the framework worker, and the
     * question is asked on the network threads -- so every answer is either
     * a wait or a guess. Comparing two numbers needs neither, and cannot be
     * late. Groups are forgotten when the player disconnects: ids are
     * reused, and a stale group would put the next holder of that id in
     * someone else's world. */
    void (*set_player_group)(int player_id, int64_t group);

    /* Puts a vehicle in a visibility group; 0 is the shared world. A packet
     * about a vehicle belongs to the VEHICLE's group, not its driver's,
     * because a car can be moved between worlds with someone sitting in it.
     * Forgotten when the vehicle is deleted. */
    void (*set_vehicle_group)(int global_id, int64_t group);

    /* The player's visibility group, 0 when it has never been set or the
     * player is unknown. */
    int64_t (*get_player_group)(int player_id);

    /* The vehicle's visibility group, 0 when it has never been set or there
     * is no such vehicle. */
    int64_t (*get_vehicle_group)(int global_id);

    /* ------------------------------------------------------------------ */
    /* Events: subscribe and emit                                         */
    /* ------------------------------------------------------------------ */

    /* Sends a named event to every connected client EXCEPT one -- the sender
     * of whatever is being relayed, typically. The fan-out is emit_all's,
     * with the excepted player as the SENDER for the relay filter
     * (relayRequest sees from = except_player_id) and visibility groups.
     * Returns 0, or -1 when except_player_id is not a connected player
     * (nothing is sent: an unknown exception is a bug in the caller, not a
     * broadcast). Inline. ABI 1.8. */
    int (*emit_others)(int except_player_id, const char* event, const char* data);

    /* ------------------------------------------------------------------ */
    /* Resources: lifecycle                                               */
    /* ------------------------------------------------------------------ */

    /* The resource's manifest as JSON: { name, version, type, server: { main
     * }, client: { files (array or null when unlisted), obfuscation (or
     * null) }, config: the [config] table of resource.toml converted as-is
     * (an empty object when absent) }. resource is a resource name, or NULL
     * for the resource making the call. Returns the length written, -1 when
     * there is no such resource or buf is too small; ask with buf = NULL for
     * the size. Read from disk on each call, so a manifest edited while the
     * server runs is seen by the next call. ABI 1.8. */
    int (*get_resource_manifest_json)(const char* resource, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* The vehicle's non-core input axes as JSON { axes: { name: value, ...
     * }, ageSeconds }. State::Inputs carries only the axes that are NOT in
     * the Pos snapshot (modded/extra inputs; the five driving controls and
     * gear are get_vehicle_controls_json) and is delta-gated -- a value is
     * sent only when it changes -- so the registry merges the deltas and
     * this is the CURRENT set. ageSeconds is since the last delta. Returns
     * the length written, -1 when there is no such vehicle or nothing
     * arrived yet; ask with buf = NULL for the size. ABI 1.9. */
    int (*get_vehicle_inputs_json)(int global_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Players                                                            */
    /* ------------------------------------------------------------------ */

    /* Everything the player is pressing, in one JSON object: { vehicle (the
     * gid driven), controls: { steering, throttle, brake, clutch,
     * parkingBrake, gear? } or null, controlsAgeSeconds, axes: { ... } (may
     * be empty), axesAgeSeconds }. Only a DRIVER's inputs reach the server,
     * so -1 when the player drives nothing (on foot, or a passenger).
     * Returns the length written, -1 when there is no such player or nothing
     * was reported yet; ask with buf = NULL for the size. ABI 1.9. */
    int (*get_player_inputs_json)(int player_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* The last State::Nodes body the vehicle's authority sent, as the JSON
     * text it sent (validated, not re-encoded): node positions / deformation
     * in the client's nodemp.sync.nodes schema. Cached for the join replay,
     * so this costs nothing extra. Returns the length written, -1 when there
     * is no such vehicle or nothing arrived yet; ask with buf = NULL for the
     * size. ABI 1.9. */
    int (*get_vehicle_nodes_json)(int global_id, char* buf, int buflen);

    /* The set of break groups reported broken on the vehicle, merged from
     * the delta stream, as a JSON array of group names. Returns the length
     * written, -1 when there is no such vehicle or nothing arrived yet; ask
     * with buf = NULL for the size. ABI 1.9. */
    int (*get_vehicle_break_groups_json)(int global_id, char* buf, int buflen);

    /* The vehicle's paints as a JSON array (the client vcf.paints schema:
     * one entry per paint layer, baseColor and the material fields), taken
     * from the cached spawn/edit config -- Paint packets are merged into it.
     * Empty array when the config carries none. -1 only for an unknown
     * vehicle. Returns the length written, -1 when there is no such vehicle
     * or nothing arrived yet; ask with buf = NULL for the size. ABI 1.9. */
    int (*get_vehicle_paints_json)(int global_id, char* buf, int buflen);

    /* Sync health of the vehicle as JSON: { authority (player id simulating
     * it, -1 none), authorityAgeSeconds (since its last accepted Pos or its
     * assignment -- the liveness heartbeat behind grace/release), seatEpoch
     * (the seat/authority transaction counter), spawnedAgeSeconds,
     * idleSeconds (0 while driven), configChangedAgeSeconds, hasPos,
     * posAgeSeconds (since the last accepted Pos), posSeq }. Returns the
     * length written, -1 when there is no such vehicle or nothing arrived
     * yet; ask with buf = NULL for the size. ABI 1.9. */
    int (*get_vehicle_sync_json)(int global_id, char* buf, int buflen);

    /* Every vehicle's last kinematic snapshot in one JSON array -- the shape
     * of get_vehicle_transform_json plus id -- taken under one registry lock
     * without copying vehicle state. Since ABI 2.0 each record also carries
     * controls (present only when hasControls is true) and electrics
     * (present only when the vehicle has sent an electrics view), so a
     * recorder or dashboard reads the whole car state in one call instead of
     * per-vehicle controls/electrics getters. Vehicles with no snapshot yet
     * are absent. Ascending by id. Returns the length written; ask with buf
     * = NULL for the size. ABI 1.9. */
    int (*get_vehicle_transforms_json)(char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Players                                                            */
    /* ------------------------------------------------------------------ */

    /* The player's focus point as JSON { pos: { x, y, z }, ageSeconds }: the
     * position the distance-based interest filter uses for this recipient --
     * its head/camera pose when it sends one, else the position of the
     * vehicle it drives. -1 when the player is unknown or no focus was ever
     * recorded. Returns the length written, -1 when there is no such player
     * or nothing was reported yet; ask with buf = NULL for the size. ABI
     * 1.9. */
    int (*get_player_focus_json)(int player_id, char* buf, int buflen);

    /* The player's last head/camera pose (State::HeadPose, ~10 Hz over UDP)
     * as JSON { pos: { x, y, z }, rot: { x, y, z, w }, freeCamera,
     * ageSeconds }. -1 when the player is unknown or never sent a pose.
     * Returns the length written, -1 when there is no such player or nothing
     * was reported yet; ask with buf = NULL for the size. ABI 1.9. */
    int (*get_player_head_pose_json)(int player_id, char* buf, int buflen);

    /* The vehicle the player's camera is on, from its last Vehicle::Camera
     * report, as JSON { vehicle (gid, -1 for none), ageSeconds }. -1 when
     * the player is unknown or never reported. Returns the length written,
     * -1 when there is no such player or nothing was reported yet; ask with
     * buf = NULL for the size. ABI 1.9. In a strict session
     * (player_set_strict, ABI 1.12) a report the server refused to relay --
     * its target is a vehicle the player does not occupy -- is recorded too,
     * with "denied": true in the JSON; the key is absent for a relayed
     * report. */
    int (*get_player_camera_json)(int player_id, char* buf, int buflen);

    /* The player's session as one JSON object: { id, name, ip, port,
     * connectedSeconds, synced (finished joining), udpConnected (the UDP
     * channel is bound), pingSeconds (since the last keepalive), unicycle
     * (the walking avatar's gid or -1), vehicle (gid occupied or -1), role
     * ("driver" | "passenger" | "none"), vehicles (count driven),
     * focusAgeSeconds (-1 when no focus), verified (the join ticket was
     * redeemed with the directory, wire v17), guest (no account behind the
     * name: a Test Drive session, or nothing verified), accountId (the
     * directory's account id, or null), accountRoles (the directory's role
     * string, "" for a guest), identifiers (["nodemp:<id>", "discord:<id>",
     * "ip:<addr>", ...] as the directory listed them; empty when
     * unverified), locale (the game's language from the mod's session:hello,
     * e.g. "ru_RU"; "" until it arrives or from a mod before 1.7.4),
     * overCapacity (joined a full server: only an explicit true from a
     * playerConnectRequest handler admits it), frozen (freeze_player) }. -1
     * for an unknown player. Returns the length written, -1 when there is no
     * such player or nothing was reported yet; ask with buf = NULL for the
     * size. ABI 1.9; the account fields since 1.10; locale, overCapacity and
     * frozen since 2.5. */
    int (*get_player_session_json)(int player_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Roles, kicks and bans                                              */
    /* ------------------------------------------------------------------ */

    /* Bans a directory account (wire v17) for FUTURE joins: a player whose
     * redeemed ticket names this account is refused at the handshake,
     * whatever address they come from. Persisted in bans.json as
     * "nodemp:<id>" beside the IP bans. Existing sessions stay (ban_player
     * kicks). Returns 0 on success, -1 for a negative id. ABI 1.10. */
    int (*ban_account)(int64_t account_id, const char* reason);

    /* Lifts an account ban. Returns 0 on success, -1 when the account was
     * not banned. ABI 1.10. */
    int (*unban_account)(int64_t account_id);

    /* 1 iff the directory account is currently banned on this server.
     * Inline. ABI 1.10. */
    int (*is_account_banned)(int64_t account_id);

    /* ------------------------------------------------------------------ */
    /* Strict session: install verification and the camera rule           */
    /* ------------------------------------------------------------------ */

    /* Asks the player's launcher helper to verify the game install again,
     * now, at `level`: NODE_VERIFY_SIZE (1, every manifest entry's length),
     * NODE_VERIFY_SCRIPTS (2, plus a hash of the game's lua/ and ui/),
     * NODE_VERIFY_FULL (3, hash everything, archives included -- slow, an
     * audit) or NODE_VERIFY_STRICT (4, the archive tables of contents
     * against the server's reference manifest, the whole game root, the user
     * folder). The request goes out as a VerifyRequest in the middle of the
     * session; the helper answers when it has finished (seconds for size,
     * longer for full), and the answer arrives as the "playerVerifyReported"
     * notification -- with the server's own verdict already applied: a
     * "differs" or "failed" report kicks the player when [integrity] verify
     * is not off, exactly as at join time, before the event fires. Returns 0
     * when the request was sent, -1 when no player has that id, -2 when that
     * level is not available for this player -- the client's launcher is too
     * old for it, or the level was not on offer (strict needs a reference
     * manifest on the server; a level outside 1..4 is never available), -3
     * when a verification is already pending for this player: at most one
     * request per player per 60 s, and a request counts as pending for those
     * 60 s whether or not its report has arrived. The check every join runs
     * does not count: a player_verify from a playerJoined handler goes out
     * at once. Inline (the request is written directly). ABI 1.12. */
    int (*player_verify)(int player_id, int level);

    /* Marks the player's session strict (on nonzero) or lifts the mark (0).
     * A strict session changes one relay rule, outbound only: a
     * Vehicle::Camera frame FROM the player that targets a vehicle it does
     * not drive or ride in is refused -- not relayed, so nobody sees it
     * spectating. Frames from other players to it are unaffected. A refused
     * frame still updates get_player_camera_json and fires
     * "playerCameraChanged", and the record then carries "denied": true (the
     * key is absent otherwise), so an observer can tell a spectate attempt
     * from a switch to an own car. Server-side only: it tells the client
     * nothing by itself -- the resource sends its own session config for
     * that. Per session, cleared at disconnect. Returns 0 on success, -1
     * when no player has that id. Inline. ABI 1.12. */
    int (*player_set_strict)(int player_id, int on);

    /* ------------------------------------------------------------------ */
    /* Server: info, settings, time and metrics                           */
    /* ------------------------------------------------------------------ */

    /* The wire protocol version this server speaks (Protocol.h ProtoVersion;
     * 21 at ABI 2.1). The launcher must match it exactly to join; the game
     * mod is installed separately and states its own version in the
     * session:hello event, which the core judges itself since wire v21 (an
     * older number is kicked as an outdated mod). A resource that wants its
     * own version check compares against THIS value, never a literal of its
     * own: the former nodemp-relay resource carried one, left at 16 while
     * the protocol went to 19, and kicked every current mod. Inline. ABI
     * 2.1. */
    int (*get_proto_version)(void);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* Hands a plugin-locked vehicle back to its spawner's Session::Policy
     * (wire v21): the spawner's current lock mode with the spawner
     * whitelisted when it is online, "unlocked" otherwise; broadcast as
     * Vehicle::Lock and fires "vehicleLockChanged" like set_vehicle_lock.
     * The lock is policy-owned again afterwards. Returns 0 on success, -1
     * for an unknown vehicle. Inline. ABI 2.2. */
    int (*release_vehicle_lock)(int global_id);

    /* ------------------------------------------------------------------ */
    /* Players                                                            */
    /* ------------------------------------------------------------------ */

    /* The player's vehicle policy (Session::Policy, wire v21) as JSON {
     * lockMode, allowTrigger, allowGrab }: lockMode "unlocked" | "locked" |
     * "driver-only" (the lock the server puts on the cars this player
     * spawns), allowTrigger / allowGrab whether other players may press its
     * cars' triggers / grab their nodes. An EMPTY object {} while the client
     * has not sent a policy yet -- the server's defaults are not the
     * player's choice. Returns the length written, -1 for an unknown player.
     * Ask with buf = NULL for the size. Fires "playerPolicyChanged" on every
     * accepted packet. ABI 2.2. */
    int (*get_player_policy_json)(int player_id, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* Resets (repairs) the vehicle from the server: exactly what a driver's
     * Vehicle::Reset does -- the damage cache is cleared and Vehicle::Reset
     * {gid, posrot} is broadcast to EVERY client, the sync authority
     * included (it performs the physical reset; the others snap) -- then the
     * "vehicleReset" notification fires with player_id -1 and data.cause =
     * "server". transform_json is
     * {"pos":{"x","y","z"},"rot":{"x","y","z","w"}?} (no rot = identity) or
     * NULL/empty for "where it stands": the last accepted position snapshot.
     * Never asks a hook: the server does not veto itself. Returns 0 on
     * success, -1 for an unknown vehicle, an unreadable transform, or no
     * transform and no snapshot yet. Inline. ABI 2.2. */
    int (*reset_vehicle)(int global_id, const char* transform_json);

    /* Spawns a car for the player from a server-supplied config (issue #14):
     * config_json is a client spawn config ({"jbm", "vcf": {"parts", "vars",
     * "paints", ...}}), transform_json
     * {"pos":{"x","y","z"},"rot":{"x","y","z","w"}?} or NULL (no rot =
     * identity; no transform = whatever pos/rot the config carries). The
     * transform is written INTO the cached config as the arrays a client's
     * own config carries, so a late joiner's replay is placed. The player
     * becomes spawner, driver and sync authority; every client gets
     * Vehicle::Spawn with spawner_id = player, then ConfigHash, Driver,
     * Authority, and the player's client is seated (PlayerVehicle) -- its
     * walking avatar is deleted like for any seat, and a car it was driving
     * is left first. cars_per_player applies as for the player's own spawn;
     * the spawner's Session::Policy lock is applied; "vehicleSpawned" fires.
     * "vehicleSpawnRequest" is NOT asked: the server does not veto itself.
     * Returns the global id (>= 0), or -1 (player unknown / not joined), -2
     * (cars_per_player reached), -3 (unreadable config, no jbm, a unicycle,
     * an unreadable transform, or over MaxVehicleConfigJson). Inline. ABI
     * 2.2. */
    int (*spawn_vehicle_for_player)(int player_id, const char* config_json, const char* transform_json);

    /* ------------------------------------------------------------------ */
    /* Server: info, settings, time and metrics                           */
    /* ------------------------------------------------------------------ */

    /* Writes {"lua": <bytes of the calling resource's Lua state, 0 for a
     * native module>, "luaAll": <bytes of every loaded resource's state
     * together>, "process": <the process's resident set in bytes; 0 where no
     * source exists>} (issue #23). lua/luaAll are
     * lua_gc(LUA_GCCOUNT/LUA_GCCOUNTB), read on the worker where every state
     * lives -- call it from a handler, a timer or a job completion. process
     * is GetProcessMemoryInfo().WorkingSetSize on Windows and
     * /proc/self/statm resident pages on Linux. Tells "my cache leaks" from
     * "the server leaks". Returns the JSON length or -1 when buf is too
     * small. ABI 2.3. */
    int (*get_memory_json)(char* buf, int buflen);

    /* Writes {"os": "windows" | "linux" | "macos", "arch": "x64" | "arm64",
     * "version": <the server's version string>} (issue #23) -- constant for
     * the process's lifetime; also carried by get_metrics_json as
     * "platform". So a resource knows whether to say db.dll or db.so, which
     * path separators to expect, and what a report was made on. Returns the
     * JSON length or -1 when buf is too small. ABI 2.3. */
    int (*get_platform_json)(char* buf, int buflen);

    /* Writes the caller's handler timing since start or the last reset
     * (issue #23): {"count", "min", "max", "mean", "stddev" (milliseconds of
     * the code's OWN time -- a node.async resume inside a timer is charged
     * to the resume, the timer keeps its share), "over" (runs past the stall
     * threshold), "thresholdMs" (250), "byKind": {"event": {...}, "timer":
     * {...}, "async task": {...}, ...}}. The same clock the stall watchdog
     * reads: over grows exactly when the log says "stalled the thread", and
     * mean/max show how close a handler runs to it before it does. Per
     * calling resource; a native module's callbacks pool under one anonymous
     * owner. reset nonzero zeroes the numbers after reading them -- on the
     * call that WRITES; a size query (buf NULL) never resets. Returns the
     * JSON length or -1 when buf is too small. ABI 2.3. */
    int (*get_handler_stats_json)(int reset, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Vehicles: state, live and cached                                   */
    /* ------------------------------------------------------------------ */

    /* get_vehicle_transforms_json keeping only what the caller reads. select
     * is JSON: an array of record keys, or an object with "fields" (an array
     * of keys; members "1", "2", ... holding a key count the same, which is
     * how a Lua table mixing both arrives), "controls" (true/false),
     * "electrics" (true, false, or an array of electrics keys to copy; an
     * array includes the electrics part) and "ids" (an array of vehicle ids;
     * every other vehicle is skipped). Keys: pos, rot, vel, angVel, time,
     * ping, seq, paused, transitioning, teleport, velocityStep, hasControls,
     * ageSeconds, controls, electrics; id is always written, and a key this
     * server does not know is ignored, so a resource written for a newer
     * server still gets the rest. NULL, "" or {} = every key, exactly
     * get_vehicle_transforms_json. Every vehicle is read under one registry
     * lock and only the requested electrics keys are copied. Returns the
     * length written; ask with buf = NULL for the size; -1 when select is
     * not JSON. ABI 2.4. */
    int (*get_vehicle_transforms_select_json)(const char* select, char* buf, int buflen);

    /* ------------------------------------------------------------------ */
    /* Roles, kicks and bans                                              */
    /* ------------------------------------------------------------------ */

    /* ban_player for a time: the ban ends at until_unix (unix seconds), and
     * the server drops it from bans.json then -- at the next check, the next
     * list or the next start. 0 or less bans for good. Returns 0, or -1 if
     * the player is unknown. ABI 2.5. */
    int (*ban_player_until)(int id, const char* reason, int64_t until_unix);

    /* ban_ip for a time: the ban ends at until_unix (unix seconds); 0 or
     * less bans for good. Returns 0 on success, -1 for an unparsable IP. ABI
     * 2.5. */
    int (*ban_ip_until)(const char* ip, const char* reason, int64_t until_unix);

    /* ban_account for a time: the ban ends at until_unix (unix seconds); 0
     * or less bans for good. Returns 0 on success, -1 for a negative id. ABI
     * 2.5. */
    int (*ban_account_until)(int64_t account_id, const char* reason, int64_t until_unix);

    /* ------------------------------------------------------------------ */
    /* Server: info, settings, time and metrics                           */
    /* ------------------------------------------------------------------ */

    /* Stops the server the way Ctrl+C does: every player is kicked with
     * reason (NULL or "" = "Server shutdown"), serverShutdown and each
     * resource's resourceUnload("shutdown") fire, storage is flushed and the
     * process exits with code 0. delay_ms > 0 schedules the stop (logged;
     * "serverShutdownScheduled" fires with {reason, at (unix seconds), by
     * (the resource)}); a new call replaces a scheduled stop; delay_ms < 0
     * cancels it ({cancelled = true, by}). The stop itself runs on the main
     * loop within 0.1 s of its time, never on the calling thread. Returns 0,
     * or -1 when a stop is already running or there was nothing to cancel.
     * ABI 2.5. */
    int (*shutdown_server)(const char* reason, int delay_ms);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* Moves the vehicle from the server WITHOUT repairing it: Vehicle::Reset
     * {gid, posrot, "repair": false} to every client, the sync authority
     * included (it moves the car; the others snap), the damage cache kept.
     * transform_json is {"pos":{"x","y","z"},"rot":{"x","y","z","w"}?}; no
     * rot keeps the way the car faces now. Damage, electrics and cargo stay.
     * No "vehicleReset" fires: the authority's next snapshot carries the
     * teleport flag and "vehicleTeleported" fires then. A mod older than
     * 1.7.4 does not know "repair": false and repairs the car as for
     * reset_vehicle. Returns 0 on success, -1 for an unknown vehicle or an
     * unreadable transform. Inline. ABI 2.5. */
    int (*teleport_vehicle)(int global_id, const char* transform_json);

    /* ------------------------------------------------------------------ */
    /* Players                                                            */
    /* ------------------------------------------------------------------ */

    /* Freezes the player (on nonzero) or thaws it (0). While frozen the
     * server enforces it, whatever the client does: the State::Pos and
     * State::Inputs of its cars are neither cached nor relayed, so they hold
     * where they stood for everyone (the stream still counts as the
     * authority's heartbeat, so the car stays its to simulate); its spawns
     * and rebuilds are refused (SpawnDeny or a rollback), as are its edits
     * (EditDeny and a rollback), its resets and its seat claims (SeatVerdict
     * DeniedPlugin), each with the reason "You are frozen by the server".
     * The client is sent the wire event session:freeze {on}: the mod (1.7.4
     * and later) freezes the car under the player and blocks the actions
     * that would undo it. "playerFrozenChanged" fires on every change;
     * get_player_session_json carries "frozen". Per session: cleared when
     * the player leaves. Returns 0, or -1 for an unknown player. ABI 2.5. */
    int (*freeze_player)(int player_id, int on);

    /* ------------------------------------------------------------------ */
    /* Inter-resource bus                                                 */
    /* ------------------------------------------------------------------ */

    /* Asks every bus subscriber of name and collects the answers: each
     * subscriber runs now, in registration order, on the framework worker
     * thread, before this returns -- unlike emit_resource_event, which
     * queues. A Lua handler's return value is its answer, and cb(source,
     * value_json, user) is called once per handler that returned something
     * other than nil, in order: source is the answering resource, value_json
     * its value as JSON (a string answer is a JSON string). A C subscriber
     * (register_resource_event) receives the call like an emit and cannot
     * answer. The caller appears as source "native" outside a dispatch, as
     * for emit. Returns the number of answers, or -1 when called off the
     * worker thread (call it from a callback) or from inside a chain of
     * calls already 8 deep. ABI 2.6. */
    int (*call_resource_event)(const char* name, const char* data, NodeBusAnswerCallback cb, void* user);

    /* ------------------------------------------------------------------ */
    /* Language hosts and registration ownership                          */
    /* ------------------------------------------------------------------ */

    /* 1 when the host lists the resource in server.toml [resources] trusted
     * (or trusts every one with "*"), else 0. A language host that runs
     * resource code in an interpreter of its own gives an untrusted resource
     * what the Lua host does: files only inside the resource folder, no
     * processes, no environment, no exit. ABI 2.7. */
    int (*is_resource_trusted)(const char* name);

    /* ------------------------------------------------------------------ */
    /* Vehicles: control                                                  */
    /* ------------------------------------------------------------------ */

    /* Scheduled changes: whether other players' new cars and rebuilds may
     * wait on a client until its player takes them ([vehicles] queue).
     * player_id -1 sets the server's own mode, "allow" (each player's
     * setting decides) or "off" (nothing waits, every change is built as it
     * arrives); a player id sets that player alone, "allow", "off", or "" to
     * follow the server's again (per session). The clients concerned are
     * told at once through the "schedule" module of modules:manifest.
     * Returns 0, -1 for an unknown player, -2 for another mode. ABI 2.8. */
    int (*set_vehicle_queue)(int player_id, const char* mode);

    /* [vehicles] queue_max_delay_sec at run time: every client builds what
     * waits for it no later than this many seconds after the first of it
     * arrived, whatever its player chose; 0 lifts the limit. Sent to every
     * joined player at once. Returns 0, or -1 outside 0..3600. ABI 2.8. */
    int (*set_vehicle_queue_max_delay)(int seconds);

    /* Every client (player_id -1) or one builds everything waiting for it
     * now, with the wire event vehicle:applyScheduled -- before a race
     * start, say. Edits the server still holds for [vehicles]
     * edit_interval_sec reach the players first (for one player, that player
     * alone), so they are built too. Returns 0, or -1 for an unknown or not
     * yet joined player. ABI 2.8. */
    int (*apply_scheduled_vehicles)(int player_id);

    /* ------------------------------------------------------------------ */
    /* The join queue                                                     */
    /* ------------------------------------------------------------------ */

    /* The join queue as JSON: {"paused": bool, "note": the text the waiting
     * players read while it is paused, "waiting": [{"accountId", "name",
     * "roles", "ip", "installId" (null unless the directory names the
     * launcher's installation), "position" (1 = next), "ready" (its mods are
     * in the launcher's cache), "admitWhenReady" (queue_admit was called),
     * "waitedSec"}, ...]} in queue order. Fills buf per the buffer
     * convention. ABI 2.8. */
    int (*get_queue_json)(char* buf, int buflen);

    /* Moves a waiting account to position (1 = first; past the end = last);
     * the ones in between shift by one, and each player whose place changed
     * reads it within five seconds. Returns 0, or -1 when the account is not
     * waiting or position is below 1. ABI 2.8. */
    int (*queue_move)(int64_t account_id, int position);

    /* Takes a waiting account out of the queue and closes its connection
     * with reason, which the player reads (NULL or "" = "Removed from the
     * join queue"); queueLeft fires with it. To wait again the player
     * presses Play again and starts at the end. Returns 0, or -1 when the
     * account is not waiting. ABI 2.8. */
    int (*queue_remove)(int64_t account_id, const char* reason);

    /* Lets a waiting account in past max_players as soon as its mods are
     * ready -- at once when they are. Returns 0, or -1 when the account is
     * not waiting. ABI 2.8. */
    int (*queue_admit)(int64_t account_id);

    /* Pauses the queue (paused nonzero) or lets it go on (0). While it is
     * paused nobody is let in from it -- a free place stays free, newcomers
     * still queue, downloads go on -- and every waiting player reads note
     * under its position (control characters removed, 200 bytes at most).
     * Returns 0. ABI 2.8. */
    int (*queue_pause)(int paused, const char* note);

    /* The [queue] settings as they are now: {"enabled", "maxWaiting",
     * "maxWaitingPerIp", "downloadsWhileWaiting", "maxNoShows",
     * "noShowWindowMin", "noShowBarMin"}. Fills buf per the buffer
     * convention. ABI 2.8. */
    int (*get_queue_limits_json)(char* buf, int buflen);

    /* Changes [queue] settings at runtime from a JSON object with any of the
     * keys get_queue_limits_json returns; keys left out keep their value.
     * Every value must be in the range server.toml allows, or nothing
     * changes. The new limits apply to the next decision -- a player at the
     * door, a download, a free place -- and never remove anyone who already
     * waits. Not written to server.toml. Returns 0, or -1 for anything but
     * such an object. ABI 2.8. */
    int (*set_queue_limits_json)(const char* limits_json);
} NodeApi;

#if defined(_WIN32)
#define NODE_EXPORT __declspec(dllexport)
#else
#define NODE_EXPORT __attribute__((visibility("default")))
#endif

/* Declare the module's ABI. Put this once, at file scope, in exactly one
 * translation unit of the module:
 *
 *     NODE_PLUGIN_ABI()
 *
 * The loader reads it before anything else and refuses the module when the
 * major does not match, so a stale build produces a readable message instead
 * of calling whatever happens to sit at the offset it remembers. */
#define NODE_PLUGIN_ABI()                            \
    NODE_EXPORT uint32_t node_plugin_abi(void) {     \
        return NODE_ABI_VERSION;                     \
    }

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NODE_H */
