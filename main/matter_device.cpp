/*
 * Matter device implementation for Shelly 1 Gen4 (ESP32-C6).
 *
 * Endpoints are created dynamically based on script slot configuration.
 * Each script slot with a non-NONE type gets a corresponding Matter endpoint.
 * Supported endpoint types:
 *   ONOFF_TOGGLE  → OnOff Light Switch + LevelControl + ColorControl + Binding
 *   ONOFF_STATE   → OnOff Light Switch + Binding (state-follow)
 *   TEMPERATURE   → Temperature Sensor (server)
 *   ILLUMINANCE   → Illuminance Sensor (server, lux set from Lua)
 *   OCCUPANCY     → Occupancy Sensor (server)
 *   CONTACT       → Contact Sensor / BooleanState (server, set from Lua)
 *   RELAY         → OnOff Light (server, physical relay)
 *
 * Command emit to bound nodes/groups:
 *   - Scripts call endpoint.command("toggle") etc.
 *   - Data is scheduled to the CHIP thread
 *   - SwitchWorkerFunction iterates the BindingTable and sends commands
 *     directly via FindOrEstablishSession + InvokeCommandRequest.
 */

#include "matter_device.h"

extern "C" {
#include "app_config.h"
#include "hw_config.h"
#include "relay.h"
#include "ota.h"
#include "shelly_boot.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs.h"
}

#include <cmath>

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_cluster.h>
#include <esp_matter_core.h>
#include <esp_matter_ota.h>

#include <app/server/Server.h>
#include <app/clusters/bindings/binding-table.h>
#include <app/OperationalSessionSetup.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <controller/InvokeInteraction.h>
#include <controller/ReadInteraction.h>
#include <credentials/FabricTable.h>
#include <credentials/GroupDataProvider.h>
#include <transport/SessionHolder.h>
#include <platform/PlatformManager.h>
#include <platform/ThreadStackManager.h>
#include <platform/ConnectivityManager.h>
#include <lib/dnssd/platform/Dnssd.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_openthread_types.h"
#include "esp_openthread.h"
#include "esp_timer.h"
#include <openthread/instance.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>
#include <openthread/netdata.h>
#if CONFIG_OPENTHREAD_SRP_CLIENT
#include <openthread/srp_client.h>
#endif
#include <openthread/thread_ftd.h>
#include "openthread_custom_config.h"
#if OPENTHREAD_CONFIG_SRP_SERVER_ENABLE
#include <openthread/srp_server.h>
#endif

/* Default config macros are NOT provided by esp_openthread.h in esp-matter/
 * esp-idf — canonical Shelly Thread firmware defines them locally. Same
 * values here: native 802.15.4 radio (ESP32-C6), no host bridge,
 * NVS partition for SRP key storage. */
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }
#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif

static const char *TAG = "matter_dev";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::cluster;
using namespace esp_matter::endpoint;
using namespace chip;
using namespace chip::app::Clusters;

/* The Matter OTA requestor applies images with esp_ota_set_boot_partition(),
 * which is exactly what the ESP-IDF bootloader needs. On devices still running
 * the stock Shelly OS loader (older installs) that loader ignores the IDF
 * otadata format, so mirror the applied slot into its SH0S boot-select too. */
static void shelly_ota_apply_handler(const chip::DeviceLayer::ChipDeviceEvent *event,
                                     intptr_t /*arg*/)
{
    if (event->Type != chip::DeviceLayer::DeviceEventType::kOtaStateChanged) return;
    if (event->OtaStateChanged.newState != chip::DeviceLayer::kOtaApplyComplete) return;
    if (!shelly_loader_present()) return;   /* ESP-IDF bootloader: nothing to do */

    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!boot) return;
    int slot = (int)boot->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    esp_err_t se = shelly_boot_switch_slot(slot);
    ESP_LOGI(TAG, "Matter OTA apply: SH0S boot-select app_%d -> %s",
             slot, esp_err_to_name(se));
}

/* Dynamic endpoint tracking — indexed by script slot */
static uint16_t s_slot_endpoints[SCRIPT_MAX_SLOTS] = {0};

/* Electrical Power Measurement endpoint (Shelly 1PM Gen4 only; 0 = not present) */
static uint16_t s_pm_endpoint[2] = { 0, 0 };   /* [0]=ch A, [1]=ch B (2PM) */
static script_slot_type_t s_slot_types[SCRIPT_MAX_SLOTS] = {SLOT_TYPE_NONE};
static uint8_t s_num_slots = 0;


/* ---------------- Binding-mediated command emit ---------------- */

struct BindingCommandData
{
    chip::EndpointId localEndpointId = 1;
    chip::CommandId  commandId       = 0;
    chip::ClusterId  clusterId       = 0;
    bool             isGroup         = false;
    /* level-control payload */
    uint8_t moveMode = 0; /* 0 = up, 1 = down */
    uint8_t rate     = 50;
    uint8_t level    = 0; /* for MoveToLevel */
    uint16_t transitionTime = 0; /* 1/10th seconds */
    /* color-temperature payload */
    uint16_t colorTempMireds = 0;
    uint16_t colorTempRate   = 0;
};


/* ------------- Multicast helpers (no CASE session needed) ------------- */

/* Dump the switch's own group security state for one fabric.  Called when a
 * multicast Invoke fails so we can see WHY GetKeyContext() returned nullptr
 * (err=d8 / CHIP_ERROR_NOT_FOUND): missing GroupKeyMap entry, missing KeySet,
 * or a keyset with no usable key. Reads exactly what GetKeyContext() uses. */
static void dump_group_state(chip::FabricIndex fabric, chip::GroupId group)
{
    using chip::Credentials::GroupDataProvider;
    GroupDataProvider *p = chip::Credentials::GetGroupDataProvider();
    if (p == nullptr) {
        ESP_LOGE(TAG, "  GROUP-DIAG: GroupDataProvider is null");
        return;
    }

    ESP_LOGW(TAG, "  GROUP-DIAG fabric=%u group=0x%04X:", fabric, group);

    // GroupKeyMap: group_id -> keyset_id
    auto *itMap = p->IterateGroupKeys(fabric);
    if (itMap) {
        GroupDataProvider::GroupKey gk;
        bool any = false;
        while (itMap->Next(gk)) {
            any = true;
            ESP_LOGW(TAG, "    GroupKeyMap: group=0x%04X -> keyset=%u", gk.group_id, gk.keyset_id);
        }
        if (!any) ESP_LOGE(TAG, "    GroupKeyMap: EMPTY (no group->keyset mapping!)");
        itMap->Release();
    } else {
        ESP_LOGE(TAG, "    GroupKeyMap: iterator null");
    }

    // KeySets stored for this fabric
    auto *itKs = p->IterateKeySets(fabric);
    if (itKs) {
        GroupDataProvider::KeySet ks;
        bool any = false;
        while (itKs->Next(ks)) {
            any = true;
            ESP_LOGW(TAG, "    KeySet id=%u num_keys=%u policy=%u",
                     ks.keyset_id, ks.num_keys_used, (unsigned) ks.policy);
        }
        if (!any) ESP_LOGE(TAG, "    KeySet: NONE stored");
        itKs->Release();
    } else {
        ESP_LOGE(TAG, "    KeySet: iterator null");
    }

    // The exact call the sender makes to encrypt the group message
    auto *kc = p->GetKeyContext(fabric, group);
    if (kc) {
        ESP_LOGW(TAG, "    GetKeyContext: OK (key found)");
        kc->Release();
    } else {
        ESP_LOGE(TAG, "    GetKeyContext: NULL -> this is the err=d8 cause");
    }
}

/* The SENDER must also have the group registered in its own GroupInfo table:
 * SessionManager::PrepareMessage calls GetGroupInfo() to build the multicast
 * address, and returns CHIP_ERROR_NOT_FOUND (err=d8) if it's missing.  The
 * setup script only runs Groups::AddGroup on the lamps, never on the switch,
 * so we register the group here (idempotent).  Uses default flags = per-group
 * multicast address, matching the lamp's AddGroup entry. */
static void ensure_group_info(chip::FabricIndex fabric, chip::GroupId group)
{
    using chip::Credentials::GroupDataProvider;
    GroupDataProvider *p = chip::Credentials::GetGroupDataProvider();
    if (p == nullptr) return;

    GroupDataProvider::GroupInfo info;
    if (p->GetGroupInfo(fabric, group, info) == CHIP_NO_ERROR) return;  // already present

    GroupDataProvider::GroupInfo newInfo(group, "grp");
    CHIP_ERROR err = p->SetGroupInfo(fabric, newInfo);
    ESP_LOGI(TAG, "ensure_group_info: register group 0x%04X fabric=%u -> err=%" CHIP_ERROR_FORMAT,
             group, fabric, err.Format());
}

static void send_onoff_multicast(const BindingCommandData &d, const Binding::TableEntry &b)
{
    ensure_group_info(b.fabricIndex, b.groupId);
    auto *em = &chip::Server::GetInstance().GetExchangeManager();
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (d.commandId == OnOff::Commands::Toggle::Id) {
        OnOff::Commands::Toggle::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == OnOff::Commands::On::Id) {
        OnOff::Commands::On::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == OnOff::Commands::Off::Id) {
        OnOff::Commands::Off::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    }
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "send_onoff_multicast FAILED: fabric=%u group=0x%04X cmd=0x%lx err=%" CHIP_ERROR_FORMAT,
                 b.fabricIndex, b.groupId, (unsigned long)d.commandId, err.Format());
        dump_group_state(b.fabricIndex, b.groupId);
    } else {
        ESP_LOGI(TAG, "send_onoff_multicast OK: fabric=%u group=0x%04X cmd=0x%lx",
                 b.fabricIndex, b.groupId, (unsigned long)d.commandId);
    }
}

static void send_level_multicast(const BindingCommandData &d, const Binding::TableEntry &b)
{
    ensure_group_info(b.fabricIndex, b.groupId);
    auto *em = &chip::Server::GetInstance().GetExchangeManager();
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (d.commandId == LevelControl::Commands::MoveWithOnOff::Id) {
        LevelControl::Commands::MoveWithOnOff::Type cmd;
        cmd.moveMode = (d.moveMode == 0) ? LevelControl::MoveModeEnum::kUp
                                         : LevelControl::MoveModeEnum::kDown;
        cmd.rate.SetNonNull(d.rate);
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == LevelControl::Commands::MoveToLevelWithOnOff::Id) {
        LevelControl::Commands::MoveToLevelWithOnOff::Type cmd;
        cmd.level = d.level;
        cmd.transitionTime.SetNonNull(d.transitionTime);
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == LevelControl::Commands::Stop::Id) {
        LevelControl::Commands::Stop::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    }
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "send_level_multicast FAILED: fabric=%u group=0x%04X err=%" CHIP_ERROR_FORMAT,
                 b.fabricIndex, b.groupId, err.Format());
    } else {
        ESP_LOGI(TAG, "send_level_multicast OK: fabric=%u group=0x%04X",
                 b.fabricIndex, b.groupId);
    }
}

static void send_colorcontrol_multicast(const BindingCommandData &d, const Binding::TableEntry &b)
{
    ensure_group_info(b.fabricIndex, b.groupId);
    auto *em = &chip::Server::GetInstance().GetExchangeManager();
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (d.commandId == ColorControl::Commands::MoveToColorTemperature::Id) {
        ColorControl::Commands::MoveToColorTemperature::Type cmd;
        cmd.colorTemperatureMireds = d.colorTempMireds;
        cmd.transitionTime = 0;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == ColorControl::Commands::MoveColorTemperature::Id) {
        ColorControl::Commands::MoveColorTemperature::Type cmd;
        cmd.moveMode = (d.moveMode == 0)
            ? ColorControl::HueMoveMode::kUp
            : ColorControl::HueMoveMode::kDown;
        cmd.rate = d.colorTempRate;
        cmd.colorTemperatureMinimumMireds = 0;
        cmd.colorTemperatureMaximumMireds = 0;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == ColorControl::Commands::StopMoveStep::Id) {
        ColorControl::Commands::StopMoveStep::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    }
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "send_colorcontrol_multicast FAILED: fabric=%u group=0x%04X err=%" CHIP_ERROR_FORMAT,
                 b.fabricIndex, b.groupId, err.Format());
    } else {
        ESP_LOGI(TAG, "send_colorcontrol_multicast OK: fabric=%u group=0x%04X cmd=0x%lx",
                 b.fabricIndex, b.groupId, (unsigned long)d.commandId);
    }
}

/* ------------- Direct-send via FindOrEstablishSession ------------- */
/*
 * Uses FindOrEstablishSession directly for unicast commands, so that
 * in the OnDeviceConnected callback we immediately send the command
 * via InvokeCommandRequest.
 *
 * FindOrEstablishSession hands back a cached CASE session, which may have died
 * without either side noticing (peer rebooted, changed its IPv6 address, or the
 * route went away). MRP only concludes that after five retransmissions, ~35 s,
 * by which time the command is long irrelevant for a wall switch. So the invoke
 * carries a short response timeout and the command is sent once more over a
 * freshly established session.
 *
 * Resending is only safe when the peer never got the first attempt: Toggle is
 * not idempotent, and a duplicate reaching the lamp shows up as "switches on
 * and immediately off again". Two things guard against that:
 *
 *   - the peer must not have acknowledged the first attempt. Every received
 *     message, including a bare MRP ack, bumps the session's peer-activity
 *     timestamp, so a timestamp newer than our send means the command did
 *     arrive and only its response was lost.
 *   - the dead session is evicted instead of marked defunct. Defunct leaves the
 *     original message in the MRP retransmit table and flips back to active as
 *     soon as the peer reappears, so the first attempt still gets delivered
 *     alongside the retry. Eviction aborts the exchange and clears that table.
 *
 * Those two guards are also what makes a short timeout safe. It is well below
 * the peer's MRP budget, so a lamp that is merely slow may well be reached by
 * the original attempt - but then it acked us and the guard above suppresses
 * the resend, leaving a single command either way. The timeout is therefore set
 * by how long a wall switch may feel unresponsive, not by MRP.
 */
static constexpr uint32_t kInvokeResponseTimeoutMs = 1500;

struct DirectSendCtx {
    BindingCommandData cmd;
    Binding::TableEntry entry;
    chip::Callback::Callback<chip::OnDeviceConnected> connCb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> failCb;
    chip::SessionHolder session;
    /* Peer activity seen when the command went out; zero if nothing was sent. */
    chip::System::Clock::Timestamp peerActivityAtSend = chip::System::Clock::kZero;
    bool retried = false;

    DirectSendCtx(const BindingCommandData &d, const Binding::TableEntry &e)
        : cmd(d), entry(e),
          connCb(OnConn, this), failCb(OnFail, this) {}

    chip::ScopedNodeId Peer() const {
        return chip::ScopedNodeId(entry.nodeId, entry.fabricIndex);
    }

    chip::System::Clock::Timestamp PeerActivity() const {
        auto handle = session.Get();
        if (handle.HasValue() && handle.Value()->IsSecureSession()) {
            return handle.Value()->AsSecureSession()->GetLastPeerActivityTime();
        }
        return chip::System::Clock::kZero;
    }

    template <typename CommandT>
    CHIP_ERROR Send(chip::Messaging::ExchangeManager &em,
                    const chip::SessionHandle &sh, const CommandT &c)
    {
        DirectSendCtx *self = this;
        peerActivityAtSend  = PeerActivity();
        return chip::Controller::InvokeCommandRequest(
            &em, sh, entry.remote, c,
            [self](const chip::app::ConcreteCommandPath & path,
                   const chip::app::StatusIB & /*status*/,
                   const auto & /*response*/) {
                ESP_LOGI(TAG, "Invoke success ep=%u cmd=0x%lx",
                         path.mEndpointId, (unsigned long) path.mCommandId);
                chip::Platform::Delete(self);
            },
            [self](CHIP_ERROR err) { self->OnInvokeFailure(err); },
            chip::NullOptional,
            chip::MakeOptional(chip::System::Clock::Milliseconds32(kInvokeResponseTimeoutMs)));
    }

    void OnInvokeFailure(CHIP_ERROR err)
    {
        const chip::ScopedNodeId peer = Peer();
        ESP_LOGW(TAG, "Invoke failure [%u:0x%llx]: %" CHIP_ERROR_FORMAT,
                 peer.GetFabricIndex(),
                 (unsigned long long) peer.GetNodeId(), err.Format());

        if (retried) {
            ESP_LOGE(TAG, "DirectSend: giving up, the retry over a fresh session failed too");
            chip::Platform::Delete(this);
            return;
        }

        /* Held for as long as the raw pointer below is used. */
        auto handle = session.Get();
        chip::Transport::SecureSession *secure = nullptr;
        if (handle.HasValue() && handle.Value()->IsSecureSession()) {
            secure = handle.Value()->AsSecureSession();
        }

        if (peerActivityAtSend != chip::System::Clock::kZero && secure != nullptr &&
            secure->GetLastPeerActivityTime() > peerActivityAtSend) {
            ESP_LOGW(TAG, "DirectSend: peer acked the command but never answered, "
                          "not resending to avoid a duplicate");
            chip::Platform::Delete(this);
            return;
        }
        retried = true;

        if (secure != nullptr) {
            /* Evict, do not just mark defunct: eviction aborts the exchange and
             * clears the MRP retransmit table, so the timed-out attempt can no
             * longer reach the peer next to the retry. */
            secure->MarkForEviction();
        }
        session.Release();

        ESP_LOGW(TAG, "DirectSend: session was dead, aborted the attempt and retrying over a fresh CASE session");
        /* Deferred: the failure can reach us from inside a session-setup
         * callback, which must not re-enter FindOrEstablishSession. */
        CHIP_ERROR sched = chip::DeviceLayer::PlatformMgr().ScheduleWork(
            RetryWorker, reinterpret_cast<intptr_t>(this));
        if (sched != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "DirectSend: cannot schedule retry: %" CHIP_ERROR_FORMAT, sched.Format());
            chip::Platform::Delete(this);
        }
    }

    static void RetryWorker(intptr_t arg)
    {
        auto *self = reinterpret_cast<DirectSendCtx *>(arg);
        chip::Server::GetInstance().GetCASESessionManager()->
            FindOrEstablishSession(self->Peer(), &self->connCb, &self->failCb);
    }

    static void OnConn(void *raw, chip::Messaging::ExchangeManager &em,
                       const chip::SessionHandle &sh)
    {
        auto *self = static_cast<DirectSendCtx *>(raw);
        auto &d = self->cmd;
        auto &b = self->entry;
        ESP_LOGI(TAG, "DirectSend: session ready -> remote ep %u cluster 0x%lx cmd 0x%lx",
                 b.remote, (unsigned long)d.clusterId, (unsigned long)d.commandId);

        self->session.Grab(sh);

        bool dispatched = true;
        CHIP_ERROR err = CHIP_NO_ERROR;

        if (d.clusterId == OnOff::Id) {
            if (d.commandId == OnOff::Commands::Toggle::Id) {
                OnOff::Commands::Toggle::Type c;
                err = self->Send(em, sh, c);
            } else if (d.commandId == OnOff::Commands::On::Id) {
                OnOff::Commands::On::Type c;
                err = self->Send(em, sh, c);
            } else if (d.commandId == OnOff::Commands::Off::Id) {
                OnOff::Commands::Off::Type c;
                err = self->Send(em, sh, c);
            } else {
                dispatched = false;
            }
        } else if (d.clusterId == LevelControl::Id) {
            if (d.commandId == LevelControl::Commands::MoveWithOnOff::Id) {
                LevelControl::Commands::MoveWithOnOff::Type c;
                c.moveMode = (d.moveMode == 0) ? LevelControl::MoveModeEnum::kUp
                                               : LevelControl::MoveModeEnum::kDown;
                c.rate.SetNonNull(d.rate);
                err = self->Send(em, sh, c);
            } else if (d.commandId == LevelControl::Commands::MoveToLevelWithOnOff::Id) {
                LevelControl::Commands::MoveToLevelWithOnOff::Type c;
                c.level = d.level;
                c.transitionTime.SetNonNull(d.transitionTime);
                err = self->Send(em, sh, c);
            } else if (d.commandId == LevelControl::Commands::Stop::Id) {
                LevelControl::Commands::Stop::Type c;
                err = self->Send(em, sh, c);
            } else {
                dispatched = false;
            }
        } else if (d.clusterId == ColorControl::Id) {
            if (d.commandId == ColorControl::Commands::MoveToColorTemperature::Id) {
                ColorControl::Commands::MoveToColorTemperature::Type c;
                c.colorTemperatureMireds = d.colorTempMireds;
                c.transitionTime = 0;
                err = self->Send(em, sh, c);
            } else if (d.commandId == ColorControl::Commands::MoveColorTemperature::Id) {
                ColorControl::Commands::MoveColorTemperature::Type c;
                c.moveMode = (d.moveMode == 0)
                    ? ColorControl::HueMoveMode::kUp
                    : ColorControl::HueMoveMode::kDown;
                c.rate = d.colorTempRate;
                c.colorTemperatureMinimumMireds = 0;
                c.colorTemperatureMaximumMireds = 0;
                err = self->Send(em, sh, c);
            } else if (d.commandId == ColorControl::Commands::StopMoveStep::Id) {
                ColorControl::Commands::StopMoveStep::Type c;
                err = self->Send(em, sh, c);
            } else {
                dispatched = false;
            }
        } else {
            dispatched = false;
        }

        if (!dispatched) {
            ESP_LOGW(TAG, "DirectSend: unsupported cluster 0x%lx cmd 0x%lx",
                     (unsigned long) d.clusterId, (unsigned long) d.commandId);
            chip::Platform::Delete(self);
        } else if (err != CHIP_NO_ERROR) {
            /* Nothing was handed to CHIP, so no callback will ever fire and
             * nothing reached the peer either. */
            self->peerActivityAtSend = chip::System::Clock::kZero;
            self->OnInvokeFailure(err);
        }
    }

    static void OnFail(void *raw, const chip::ScopedNodeId &peer, CHIP_ERROR err)
    {
        ESP_LOGE(TAG, "DirectSend: CASE failed [%u:0x%llx]: %" CHIP_ERROR_FORMAT,
                 peer.GetFabricIndex(),
                 (unsigned long long)peer.GetNodeId(),
                 err.Format());
        chip::Platform::Delete(static_cast<DirectSendCtx *>(raw));
    }
};

static void expire_peer_sessions(const chip::ScopedNodeId &peer)
{
    ESP_LOGW(TAG, "Expiring cached session to bound peer [%u:0x%llx]",
             peer.GetFabricIndex(), (unsigned long long) peer.GetNodeId());
    chip::Server::GetInstance().GetSecureSessionManager().ExpireAllSessions(peer);
}

static void expire_bound_peer_sessions()
{
    for (const auto &e : Binding::Table::GetInstance()) {
        if (e.type != Binding::MATTER_UNICAST_BINDING) {
            continue;
        }
        expire_peer_sessions(chip::ScopedNodeId(e.nodeId, e.fabricIndex));
    }
}

/* ------------- Thread prefix changes invalidate cached sessions ------------- */
/*
 * A peer's IPv6 address is resolved once, while the CASE session is being
 * established, and never refreshed for as long as that session stays in the
 * table - which can be days, since sessions are only evicted when the table
 * runs out of room. Border routers hand out a randomly generated on-mesh
 * prefix, so a border router restart re-addresses every node in the network and
 * leaves us sending to an address nobody listens on, noticed only when an
 * invoke times out.
 *
 * OpenThread reports network-data changes, which is where on-mesh prefixes
 * live, so compare the prefixes against the previous set and expire the
 * sessions to all bound peers when they differ. The first press after a prefix
 * change then pays one CASE handshake instead of an invoke timeout.
 */
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
static uint32_t s_onmesh_prefix_hash  = 0;
static bool     s_onmesh_prefix_known = false;

static uint32_t onmesh_prefix_hash()
{
    uint32_t hash = 2166136261u;   /* FNV-1a */

    chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
    otInstance *instance = esp_openthread_get_instance();
    if (instance != nullptr) {
        otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
        otBorderRouterConfig cfg;
        while (otNetDataGetNextOnMeshPrefix(instance, &it, &cfg) == OT_ERROR_NONE) {
            for (uint8_t byte : cfg.mPrefix.mPrefix.mFields.m8) {
                hash = (hash ^ byte) * 16777619u;
            }
            hash = (hash ^ cfg.mPrefix.mLength) * 16777619u;
        }
    }
    chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();

    return hash;
}

static void thread_netdata_handler(const chip::DeviceLayer::ChipDeviceEvent *event,
                                   intptr_t /*arg*/)
{
    if (event->Type != chip::DeviceLayer::DeviceEventType::kThreadStateChange) return;
    if (!event->ThreadStateChange.NetDataChanged) return;

    uint32_t hash = onmesh_prefix_hash();
    if (s_onmesh_prefix_known && hash == s_onmesh_prefix_hash) return;

    bool changed = s_onmesh_prefix_known;
    s_onmesh_prefix_hash  = hash;
    s_onmesh_prefix_known = true;
    if (!changed) return;   /* first observation, nothing cached to invalidate */

    ESP_LOGW(TAG, "Thread on-mesh prefixes changed, peer addresses are stale");
    expire_bound_peer_sessions();
}
#endif /* CHIP_DEVICE_CONFIG_ENABLE_THREAD */

/* ------------------- Reachability keepalive ------------------- */
/*
 * A prefix change is not the only way a cached session goes stale: the peer can
 * reboot, or move to another parent and take a new RLOC with it. Nothing tells
 * us, because a lamp without a subscription to us never sends anything, and CHIP
 * only refreshes a peer address from messages it receives.
 *
 * So poll: read one global attribute (ClusterRevision, two bytes) from the bound
 * cluster of every unicast peer, and drop the session when that read fails. A
 * read is idempotent, unlike Toggle, so a lost keepalive costs nothing and the
 * ReadClient is gone again as soon as the answer is in - no subscription state
 * on either side, which matters because a Matter device only has to support
 * three subscriptions per fabric and the controller needs those itself.
 *
 * The read goes to the bound endpoint and cluster on purpose: that is where the
 * controller granted us access, so it cannot fail on ACL where a command would
 * have succeeded.
 */
static constexpr uint32_t kKeepaliveDefaultSeconds = 600;
static constexpr uint8_t  kKeepaliveMaxPeers       = 8;

static uint32_t s_keepalive_seconds = kKeepaliveDefaultSeconds;

#define KEEPALIVE_NVS_NS  "ota"
#define KEEPALIVE_NVS_KEY "ka_s"

static void keepalive_expire_worker(intptr_t arg)
{
    auto *peer = reinterpret_cast<chip::ScopedNodeId *>(arg);
    expire_peer_sessions(*peer);
    chip::Platform::Delete(peer);
}

/* Deferred: the failure arrives while the ReadClient is still on the stack. */
static void keepalive_expire_later(const chip::ScopedNodeId &peer)
{
    auto *copy = chip::Platform::New<chip::ScopedNodeId>(peer);
    if (copy == nullptr) {
        return;
    }
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        keepalive_expire_worker, reinterpret_cast<intptr_t>(copy));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Keepalive: cannot schedule session eviction: %" CHIP_ERROR_FORMAT,
                 err.Format());
        chip::Platform::Delete(copy);
    }
}

struct KeepaliveCtx {
    chip::ScopedNodeId peer;
    chip::EndpointId   remote;
    chip::ClusterId    cluster;
    chip::Callback::Callback<chip::OnDeviceConnected> connCb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> failCb;

    KeepaliveCtx(const chip::ScopedNodeId &p, chip::EndpointId ep, chip::ClusterId c)
        : peer(p), remote(ep), cluster(c), connCb(OnConn, this), failCb(OnFail, this) {}

    static void OnConn(void *raw, chip::Messaging::ExchangeManager &em,
                       const chip::SessionHandle &sh)
    {
        auto *self = static_cast<KeepaliveCtx *>(raw);
        const chip::ScopedNodeId peer = self->peer;

        CHIP_ERROR err = chip::Controller::ReadAttribute<uint16_t>(
            &em, sh, self->remote, self->cluster,
            chip::app::Clusters::Globals::Attributes::ClusterRevision::Id,
            [peer](const chip::app::ConcreteDataAttributePath & /*path*/,
                   const uint16_t & /*value*/) {
                ESP_LOGD(TAG, "Keepalive: peer [%u:0x%llx] answered",
                         peer.GetFabricIndex(), (unsigned long long) peer.GetNodeId());
            },
            [peer](const chip::app::ConcreteDataAttributePath * /*path*/, CHIP_ERROR err) {
                ESP_LOGW(TAG, "Keepalive: peer [%u:0x%llx] unreachable (%" CHIP_ERROR_FORMAT
                              "), dropping its cached session",
                         peer.GetFabricIndex(), (unsigned long long) peer.GetNodeId(),
                         err.Format());
                keepalive_expire_later(peer);
            });

        if (err != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "Keepalive: read to [%u:0x%llx] not started: %" CHIP_ERROR_FORMAT,
                     peer.GetFabricIndex(), (unsigned long long) peer.GetNodeId(),
                     err.Format());
        }
        /* The callbacks above capture the peer by value, so nothing here is
         * needed once the read is handed to CHIP. */
        chip::Platform::Delete(self);
    }

    static void OnFail(void *raw, const chip::ScopedNodeId &peer, CHIP_ERROR err)
    {
        ESP_LOGW(TAG, "Keepalive: no session to [%u:0x%llx]: %" CHIP_ERROR_FORMAT,
                 peer.GetFabricIndex(), (unsigned long long) peer.GetNodeId(), err.Format());
        chip::Platform::Delete(static_cast<KeepaliveCtx *>(raw));
    }
};

static void keepalive_timer(chip::System::Layer *layer, void *arg);

static void keepalive_reschedule()
{
    chip::DeviceLayer::SystemLayer().CancelTimer(keepalive_timer, nullptr);
    if (s_keepalive_seconds == 0) {
        return;
    }
    CHIP_ERROR err = chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Seconds32(s_keepalive_seconds), keepalive_timer, nullptr);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Keepalive: cannot start timer: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void keepalive_probe_peers()
{
    chip::NodeId probed[kKeepaliveMaxPeers];
    uint8_t count = 0;

    for (const auto &e : Binding::Table::GetInstance()) {
        if (e.type != Binding::MATTER_UNICAST_BINDING) {
            continue;
        }
        /* One probe per peer, however many endpoints are bound to it. */
        bool seen = false;
        for (uint8_t i = 0; i < count; i++) {
            if (probed[i] == e.nodeId) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (count == kKeepaliveMaxPeers) {
            ESP_LOGW(TAG, "Keepalive: more than %u bound peers, skipping the rest",
                     (unsigned) kKeepaliveMaxPeers);
            break;
        }
        probed[count++] = e.nodeId;

        auto *ctx = chip::Platform::New<KeepaliveCtx>(
            chip::ScopedNodeId(e.nodeId, e.fabricIndex), e.remote,
            e.clusterId.value_or(OnOff::Id));
        if (ctx == nullptr) {
            ESP_LOGE(TAG, "Keepalive: OOM KeepaliveCtx");
            return;
        }
        chip::Server::GetInstance().GetCASESessionManager()->
            FindOrEstablishSession(ctx->peer, &ctx->connCb, &ctx->failCb);
    }
}

static void keepalive_timer(chip::System::Layer * /*layer*/, void * /*arg*/)
{
    keepalive_probe_peers();
    keepalive_reschedule();
}

static void keepalive_apply_worker(intptr_t /*arg*/)
{
    keepalive_reschedule();
}

static void keepalive_init()
{
    nvs_handle_t h;
    if (nvs_open(KEEPALIVE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0;
        if (nvs_get_u32(h, KEEPALIVE_NVS_KEY, &v) == ESP_OK) {
            s_keepalive_seconds = v;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Binding keepalive interval = %lu s (0 = off)",
             (unsigned long) s_keepalive_seconds);

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(keepalive_apply_worker, 0);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Keepalive: cannot schedule start: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

extern "C" uint32_t matter_keepalive_interval_get(void)
{
    return s_keepalive_seconds;
}

extern "C" esp_err_t matter_keepalive_interval_set(uint32_t seconds)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(KEEPALIVE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, KEEPALIVE_NVS_KEY, seconds);
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    s_keepalive_seconds = seconds;
    ESP_LOGI(TAG, "Binding keepalive interval saved: %lu s", (unsigned long) seconds);
    CHIP_ERROR cerr = chip::DeviceLayer::PlatformMgr().ScheduleWork(keepalive_apply_worker, 0);
    return (cerr == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
}

static void SwitchWorkerFunction(intptr_t context)
{
    BindingCommandData *d = reinterpret_cast<BindingCommandData *>(context);
    uint32_t sent  = 0;
    uint32_t total = 0;

    for (const auto & e : Binding::Table::GetInstance()) {
        total++;
        ESP_LOGI(TAG, "BindingTable[%lu]: type=%u local=%u remote=%u "
                 "nodeId=0x%llx group=%u cluster=0x%lx fabric=%u",
                 (unsigned long) total,
                 (unsigned) e.type, e.local, e.remote,
                 (unsigned long long) e.nodeId,
                 (unsigned) e.groupId,
                 (unsigned long) e.clusterId.value_or(0),
                 (unsigned) e.fabricIndex);

        if (e.local != d->localEndpointId) continue;
        if (e.clusterId.has_value() && e.clusterId.value() != d->clusterId) continue;

        if (e.type == Binding::MATTER_UNICAST_BINDING) {
            auto *ctx = chip::Platform::New<DirectSendCtx>(*d, e);
            if (!ctx) {
                ESP_LOGE(TAG, "SwitchWorker: OOM DirectSendCtx");
                continue;
            }
            chip::ScopedNodeId peer(e.nodeId, e.fabricIndex);
            ESP_LOGI(TAG, "SwitchWorker: FindOrEstablishSession [%u:0x%llx]",
                     peer.GetFabricIndex(),
                     (unsigned long long) peer.GetNodeId());
            chip::Server::GetInstance().GetCASESessionManager()->
                FindOrEstablishSession(peer, &ctx->connCb, &ctx->failCb);
            sent++;
        } else if (e.type == Binding::MATTER_MULTICAST_BINDING) {
            if (d->clusterId == OnOff::Id)              send_onoff_multicast(*d, e);
            else if (d->clusterId == LevelControl::Id)  send_level_multicast(*d, e);
            else if (d->clusterId == ColorControl::Id)  send_colorcontrol_multicast(*d, e);
            sent++;
        }
    }

    ESP_LOGI(TAG, "SwitchWorker: ep=%u cluster=0x%lx cmd=0x%lx total=%lu sent=%lu",
             d->localEndpointId, (unsigned long) d->clusterId,
             (unsigned long) d->commandId, (unsigned long) total, (unsigned long) sent);
    if (sent == 0) {
        ESP_LOGW(TAG, "SwitchWorker: no matching binding entries for ep=%u",
                 d->localEndpointId);
    }
    chip::Platform::Delete(d);
}

/* Hands the command to the Matter task. Owns d: on failure it is released here,
 * otherwise SwitchWorkerFunction releases it. */
static void schedule_switch_work(BindingCommandData *d)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        SwitchWorkerFunction, reinterpret_cast<intptr_t>(d));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "switch_send: cannot schedule work: %" CHIP_ERROR_FORMAT, err.Format());
        chip::Platform::Delete(d);
    }
}

static void switch_send(uint16_t local_ep, chip::ClusterId cluster, chip::CommandId cmd,
                        uint8_t move_mode = 0, uint8_t rate = 50)
{
    ESP_LOGI(TAG, "switch_send ep=%u cluster=0x%lx cmd=0x%lx",
             local_ep, (unsigned long) cluster, (unsigned long) cmd);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = local_ep;
    d->clusterId       = cluster;
    d->commandId       = cmd;
    d->moveMode        = move_mode;
    d->rate            = rate;
    schedule_switch_work(d);
}

/* ---------------- Public API (C-callable) ---------------- */

/* Guard: ep == 0 means the corresponding endpoint has not yet been created
 * by matter_start(). Defensive: early return so that spurious ISR callbacks
 * right after boot do not crash into chip:: code. */
extern "C" void matter_send_onoff_toggle(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, OnOff::Id, OnOff::Commands::Toggle::Id);
}

extern "C" void matter_send_onoff_on(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, OnOff::Id, OnOff::Commands::On::Id);
}

extern "C" void matter_send_onoff_off(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, OnOff::Id, OnOff::Commands::Off::Id);
}

extern "C" void matter_send_level_move(uint16_t ep, bool up, uint8_t rate)
{
    if (!ep) return;
    /* Some lamps (e.g. IKEA) ignore MoveWithOnOff when the lamp is off.
     * Send an explicit On first when dimming up to work around this. */
    if (up) {
        switch_send(ep, OnOff::Id, OnOff::Commands::On::Id);
    }
    switch_send(ep, LevelControl::Id, LevelControl::Commands::MoveWithOnOff::Id,
                up ? 0 : 1, rate);
}

extern "C" void matter_send_level_stop(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, LevelControl::Id, LevelControl::Commands::Stop::Id);
}

extern "C" void matter_send_level_move_to_level(uint16_t ep, uint8_t level, uint16_t transition_ds)
{
    if (!ep) return;
    ESP_LOGI(TAG, "move_to_level ep=%u level=%u transition=%u", ep, level, transition_ds);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = ep;
    d->clusterId       = LevelControl::Id;
    d->commandId       = LevelControl::Commands::MoveToLevelWithOnOff::Id;
    d->level           = level;
    d->transitionTime  = transition_ds;
    schedule_switch_work(d);
}

extern "C" void matter_send_color_temp_set(uint16_t ep, uint16_t mireds)
{
    if (!ep) return;
    ESP_LOGI(TAG, "color_temp_set ep=%u mireds=%u", ep, mireds);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = ep;
    d->clusterId       = ColorControl::Id;
    d->commandId       = ColorControl::Commands::MoveToColorTemperature::Id;
    d->colorTempMireds = mireds;
    schedule_switch_work(d);
}

extern "C" void matter_send_color_temp_move(uint16_t ep, bool warmer, uint16_t rate)
{
    if (!ep) return;
    ESP_LOGI(TAG, "color_temp_move ep=%u warmer=%d rate=%u", ep, warmer, rate);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = ep;
    d->clusterId       = ColorControl::Id;
    d->commandId       = ColorControl::Commands::MoveColorTemperature::Id;
    d->moveMode        = warmer ? 0 : 1;  /* 0=Up(warmer/higher mireds), 1=Down(cooler) */
    d->colorTempRate   = rate;
    schedule_switch_work(d);
}

extern "C" void matter_send_color_temp_stop(uint16_t ep)
{
    if (!ep) return;
    ESP_LOGI(TAG, "color_temp_stop ep=%u", ep);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = ep;
    d->clusterId       = ColorControl::Id;
    d->commandId       = ColorControl::Commands::StopMoveStep::Id;
    schedule_switch_work(d);
}



extern "C" void matter_update_temperature(int16_t centi_c)
{
    /* Matter 1.5.1 / newer connectedhomeip no longer generates the Get/Set
     * accessors here, so use the esp-matter attribute store update path (locks
     * the stack internally). MeasuredValue is a nullable int16. */
    esp_matter_attr_val_t v = esp_matter_nullable_int16(nullable<int16_t>(centi_c));
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == SLOT_TYPE_TEMPERATURE && s_slot_endpoints[i]) {
            esp_err_t err = attribute::update(s_slot_endpoints[i], TemperatureMeasurement::Id,
                TemperatureMeasurement::Attributes::MeasuredValue::Id, &v);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "temperature update EP%u failed: %s",
                         s_slot_endpoints[i], esp_err_to_name(err));
            }
        }
    }
}

extern "C" void matter_update_illuminance(float lux)
{
    /* Matter encodes illuminance as MeasuredValue = 10000*log10(lux)+1 (uint16,
     * range 1..0xFFFE); 0 means "unknown / too dark". */
    uint16_t measured;
    if (lux <= 0.0f) {
        measured = 0;
    } else {
        double enc = 10000.0 * std::log10((double)lux) + 1.0;
        if (enc < 1.0)      enc = 1.0;
        if (enc > 0xFFFE)   enc = 0xFFFE;
        measured = (uint16_t)enc;
    }
    esp_matter_attr_val_t v = esp_matter_nullable_uint16(nullable<uint16_t>(measured));
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == SLOT_TYPE_ILLUMINANCE && s_slot_endpoints[i]) {
            esp_err_t err = attribute::update(s_slot_endpoints[i], IlluminanceMeasurement::Id,
                IlluminanceMeasurement::Attributes::MeasuredValue::Id, &v);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "illuminance update EP%u failed: %s",
                         s_slot_endpoints[i], esp_err_to_name(err));
            }
        }
    }
}

extern "C" void matter_update_occupancy(bool occupied)
{
    /* Occupancy is a bitmap8; update via the esp-matter attribute store (locks
     * the stack internally) as the generated Set accessor is gone in 1.5.1. */
    esp_matter_attr_val_t v = esp_matter_bitmap8(occupied ? 1 : 0);
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == SLOT_TYPE_OCCUPANCY && s_slot_endpoints[i]) {
            esp_err_t err = attribute::update(s_slot_endpoints[i], OccupancySensing::Id,
                OccupancySensing::Attributes::Occupancy::Id, &v);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "occupancy update EP%u failed: %s",
                         s_slot_endpoints[i], esp_err_to_name(err));
            }
        }
    }
}

extern "C" void matter_update_boolean_state(uint16_t endpoint_id, bool state)
{
    if (!endpoint_id) return;
    /* The BooleanState endpoint is built as a plain ember-stored cluster (see
     * SLOT_TYPE_CONTACT), so the esp-matter attribute store update path works and
     * locks the stack internally. */
    esp_matter_attr_val_t v = esp_matter_bool(state);
    attribute::update(endpoint_id, BooleanState::Id,
                      BooleanState::Attributes::StateValue::Id, &v);
}

/* Write the four ElectricalPowerMeasurement attributes on a given endpoint.
 * Matter units: mW / mV / mA / mHz. These attributes are nullable int64.
 * The ElectricalPowerMeasurement cluster is created dynamically (not in the
 * static ZAP config), so no generated Get/Set accessors exist — use the
 * esp-matter attribute store update path (locks the stack internally). */
static void write_power_attrs(uint16_t ep, float voltage_v, float current_a,
                              float power_w, float frequency_hz)
{
    if (!ep) return;
    namespace EPM = chip::app::Clusters::ElectricalPowerMeasurement;
    /* NaN means "leave this attribute unchanged" (Lua may report power only). */
    if (!std::isnan(power_w)) {
        esp_matter_attr_val_t v = esp_matter_nullable_int64(nullable<int64_t>((int64_t)(power_w * 1000.0f)));
        attribute::update(ep, EPM::Id, EPM::Attributes::ActivePower::Id, &v);
    }
    if (!std::isnan(voltage_v)) {
        esp_matter_attr_val_t v = esp_matter_nullable_int64(nullable<int64_t>((int64_t)(voltage_v * 1000.0f)));
        attribute::update(ep, EPM::Id, EPM::Attributes::Voltage::Id, &v);
    }
    if (!std::isnan(current_a)) {
        esp_matter_attr_val_t v = esp_matter_nullable_int64(nullable<int64_t>((int64_t)(current_a * 1000.0f)));
        attribute::update(ep, EPM::Id, EPM::Attributes::ActiveCurrent::Id, &v);
    }
    if (!std::isnan(frequency_hz)) {
        esp_matter_attr_val_t v = esp_matter_nullable_int64(nullable<int64_t>((int64_t)(frequency_hz * 1000.0f)));
        attribute::update(ep, EPM::Id, EPM::Attributes::Frequency::Id, &v);
    }
}

extern "C" void matter_update_power_ch(int ch, float voltage_v, float current_a,
                                       float power_w, float frequency_hz)
{
    if (ch < 0 || ch > 1 || !s_pm_endpoint[ch]) return;
    write_power_attrs(s_pm_endpoint[ch], voltage_v, current_a, power_w, frequency_hz);
}

extern "C" void matter_update_power_ep(uint16_t endpoint_id, float voltage_v, float current_a,
                                       float power_w, float frequency_hz)
{
    /* Lua-driven SLOT_TYPE_POWER endpoint: write straight to the slot's own EP. */
    write_power_attrs(endpoint_id, voltage_v, current_a, power_w, frequency_hz);
}

extern "C" void matter_update_power(float voltage_v, float current_a,
                                    float power_w, float frequency_hz)
{
    matter_update_power_ch(0, voltage_v, current_a, power_w, frequency_hz);
}

extern "C" void matter_update_relay_onoff(int ch, bool on)
{
    script_slot_type_t want = (ch == 1) ? SLOT_TYPE_RELAY2 : SLOT_TYPE_RELAY;
    esp_matter_attr_val_t v = esp_matter_bool(on);
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == want && s_slot_endpoints[i]) {
            attribute::update(s_slot_endpoints[i],
                OnOff::Id,
                OnOff::Attributes::OnOff::Id, &v);
        }
    }
}

extern "C" void matter_disable_thread(void)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    ESP_LOGW(TAG, "Disabling Thread to free 2.4 GHz radio for WiFi");
    CHIP_ERROR err = chip::DeviceLayer::ThreadStackMgr().SetThreadEnabled(false);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "SetThreadEnabled(false): %" CHIP_ERROR_FORMAT, err.Format());
    }
#endif
}

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
/* Thread connectivity watchdog. OpenThread normally re-attaches on its own, but
 * on ESP32-C6 + Matter a node can occasionally stay DETACHED after a partition/
 * leader change or a radio-coex hiccup, becoming unreachable over Thread (HA
 * cannot drive the relay, bindings do not emit) until a manual reboot. This
 * watchdog watches the attach state and recovers automatically:
 *   - soft: toggle the Thread interface to force a fresh attach,
 *   - hard: reboot if still detached after a longer period. */
#define TWDG_PERIOD_US   (30ULL * 1000 * 1000)   /* check every 30 s */
#define TWDG_SOFT_TICKS  4                        /* ~2 min detached → toggle */
#define TWDG_HARD_TICKS  10                       /* ~5 min detached → reboot */

static esp_timer_handle_t s_twdg_timer = nullptr;
static int                s_twdg_detached_ticks = 0;

static void thread_watchdog_cb(void *)
{
    /* WiFi runtime mode intentionally disables Thread — never interfere. */
    if (ota_wifi_runtime_active()) {
        s_twdg_detached_ticks = 0;
        return;
    }

    auto &conn = chip::DeviceLayer::ConnectivityMgr();
    if (!conn.IsThreadProvisioned() || !conn.IsThreadEnabled()) {
        s_twdg_detached_ticks = 0;   /* not on Thread / intentionally off */
        return;
    }
    if (conn.IsThreadAttached()) {
        if (s_twdg_detached_ticks)
            ESP_LOGI(TAG, "Thread re-attached — watchdog reset");
        s_twdg_detached_ticks = 0;
        return;
    }

    s_twdg_detached_ticks++;
    ESP_LOGW(TAG, "Thread detached — watchdog tick %d", s_twdg_detached_ticks);

    /* Temporary WiFi shares the one radio, so Thread wins the moment it
     * suffers: closing the window gives back the router role and lets the
     * re-attach happen on a Thread-only radio. */
    if (ota_wifi_coex_seconds_left() > 0) {
        ESP_LOGW(TAG, "Thread detached while temporary WiFi is on — closing the WiFi window");
        ota_wifi_coex_stop();
        return;
    }

    if (s_twdg_detached_ticks == TWDG_SOFT_TICKS) {
        ESP_LOGW(TAG, "Thread detached ~%d s — toggling interface to force re-attach",
                 (int)(TWDG_SOFT_TICKS * 30));
        CHIP_ERROR err = chip::DeviceLayer::ThreadStackMgr().SetThreadEnabled(false);
        if (err == CHIP_NO_ERROR) {
            err = chip::DeviceLayer::ThreadStackMgr().SetThreadEnabled(true);
        }
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Thread interface toggle failed: %" CHIP_ERROR_FORMAT, err.Format());
        }
    } else if (s_twdg_detached_ticks >= TWDG_HARD_TICKS) {
        ESP_LOGE(TAG, "Thread still detached ~%d s — rebooting to recover",
                 (int)(TWDG_HARD_TICKS * 30));
        esp_restart();
    }
}
#endif

extern "C" void matter_thread_watchdog_start(void)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    if (s_twdg_timer) return;
    const esp_timer_create_args_t args = {
        .callback = &thread_watchdog_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "thread_wdg",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_twdg_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Thread watchdog: timer create failed");
        return;
    }
    esp_timer_start_periodic(s_twdg_timer, TWDG_PERIOD_US);
    ESP_LOGI(TAG, "Thread connectivity watchdog started (soft ~2 min, hard ~5 min)");
#endif
}

#if OPENTHREAD_CONFIG_SRP_SERVER_ENABLE
/* Fallback SRP server: only fills the void when no real Thread Border Router
 * is present. A BR (e.g. Google Nest) runs the authoritative SRP server AND an
 * advertising proxy that bridges registrations to the LAN mDNS — without which
 * an off-mesh controller (matter.js/HA) cannot discover any Matter device.
 * Our node has no advertising proxy, so if it wins the SRP anycast election it
 * silently hides the whole mesh from the controller. We therefore run the SRP
 * server only as a fallback and yield the moment a border router appears.
 *
 * Detection of a border router keys on external routes / on-mesh prefixes in
 * the Thread Network Data — things a BR publishes (OMR prefix, on-link/default
 * routes) but a plain SRP-server node does not. Detection is deliberately
 * biased toward "BR present" (yield) — the dangerous failure is falsely
 * enabling and hijacking mesh-wide discovery.
 *
 * When no BR is present, multiple fallback nodes elect a SINGLE active server
 * so they don't all enable at once and keep toggling each other:
 *   - Each node reads the DNS/SRP server entries other nodes advertise in the
 *     Thread Network Data. The node with the LOWEST RLOC16 wins; higher-RLOC
 *     nodes yield (debounced, so a brief netdata hiccup doesn't cause flapping).
 *   - Enabling is delayed by a base debounce PLUS a per-node RANDOM jitter, so
 *     two nodes rarely activate in the same instant during the initial gap.
 * The RLOC ordering is a stable total order, so convergence is deterministic
 * (lowest RLOC stays on) regardless of who happened to start first. */
#define SRP_EVAL_PERIOD_US        (5 * 1000 * 1000)  /* re-evaluate every 5 s */
#define SRP_NO_BR_DEBOUNCE_TICKS  6                  /* ~30 s base delay w/o BR */
#define SRP_JITTER_TICKS          6                  /* +0..30 s random stagger */
#define SRP_YIELD_DEBOUNCE_TICKS  3                  /* ~15 s before yielding to peer */
#define THREAD_ENTERPRISE_NUMBER  44970u             /* DNS/SRP service enterprise no. */

static bool             s_srp_server_enabled = false;
static bool             s_srp_fallback_active = false;
static bool             s_srp_fallback_paused = false;
static esp_timer_handle_t s_srp_eval_timer = nullptr;
static int              s_srp_no_br_ticks = 0;
static int              s_srp_yield_ticks = 0;
static int              s_srp_enable_target = SRP_NO_BR_DEBOUNCE_TICKS;

static void srp_pick_enable_target(void)
{
    s_srp_enable_target = SRP_NO_BR_DEBOUNCE_TICKS +
                          (int)(esp_random() % (SRP_JITTER_TICKS + 1));
}

/* Must be called with the Thread stack lock held. */
static bool other_border_router_present(otInstance *instance)
{
    uint16_t self_rloc = otThreadGetRloc16(instance);

    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otExternalRouteConfig route;
    while (otNetDataGetNextRoute(instance, &it, &route) == OT_ERROR_NONE) {
        if (route.mRloc16 != self_rloc)
            return true;
    }

    it = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig prefix;
    while (otNetDataGetNextOnMeshPrefix(instance, &it, &prefix) == OT_ERROR_NONE) {
        if (prefix.mRloc16 != self_rloc)
            return true;
    }
    return false;
}

/* Must be called with the Thread stack lock held. Returns the lowest RLOC16 of
 * any OTHER DNS/SRP server advertised in the Thread Network Data, or 0xFFFF if
 * none. DNS/SRP service = Thread enterprise number 44970, service-data byte 0
 * of 0x5c (anycast) or 0x5d (unicast). */
static uint16_t lowest_other_srp_server_rloc(otInstance *instance)
{
    uint16_t self = otThreadGetRloc16(instance);
    uint16_t best = 0xFFFF;

    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otServiceConfig svc;
    while (otNetDataGetNextService(instance, &it, &svc) == OT_ERROR_NONE) {
        if (svc.mEnterpriseNumber != THREAD_ENTERPRISE_NUMBER) continue;
        if (svc.mServiceDataLength < 1) continue;
        uint8_t sn = svc.mServiceData[0];
        if (sn != 0x5c && sn != 0x5d) continue;
        uint16_t rloc = svc.mServerConfig.mRloc16;
        if (rloc == self) continue;
        if (rloc < best) best = rloc;
    }
    return best;
}

/* Must be called with the Thread stack lock held. */
static void srp_set_enabled_locked(otInstance *instance, bool enable)
{
    if (enable == s_srp_server_enabled) return;
    otSrpServerSetEnabled(instance, enable);
    s_srp_server_enabled = enable;
    ESP_LOGW(TAG, "SRP fallback server %s", enable
             ? "ENABLED (no border router — elected as lowest RLOC)"
             : "DISABLED (border router present or lower-RLOC peer — yielding)");
}

static void srp_eval_cb(void *)
{
    chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
    otInstance *instance = esp_openthread_get_instance();
    if (instance) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        bool eligible = (role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER);

        if (other_border_router_present(instance)) {
            /* Real border router present → always yield immediately. */
            s_srp_no_br_ticks = 0;
            s_srp_yield_ticks = 0;
            srp_pick_enable_target();                  /* fresh delay for next gap */
            srp_set_enabled_locked(instance, false);
        } else {
            uint16_t self  = otThreadGetRloc16(instance);
            uint16_t other = lowest_other_srp_server_rloc(instance);
            bool peer_wins = (other != 0xFFFF && other < self);

            if (peer_wins) {
                /* A lower-RLOC fallback server should be the single active one. */
                s_srp_no_br_ticks = 0;
                if (s_srp_server_enabled) {
                    if (++s_srp_yield_ticks >= SRP_YIELD_DEBOUNCE_TICKS) {
                        srp_set_enabled_locked(instance, false);
                        s_srp_yield_ticks = 0;
                    }
                }
            } else {
                /* We are the lowest-RLOC candidate (or the only one). */
                s_srp_yield_ticks = 0;
                if (s_srp_no_br_ticks < s_srp_enable_target) s_srp_no_br_ticks++;
                if (eligible && s_srp_no_br_ticks >= s_srp_enable_target)
                    srp_set_enabled_locked(instance, true);
            }
        }
    }
    chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();
}
#endif

extern "C" esp_err_t matter_srp_server_start(void)
{
#if OPENTHREAD_CONFIG_SRP_SERVER_ENABLE
    if (s_srp_fallback_active) return ESP_OK;

    srp_pick_enable_target();   /* per-node random enable delay to stagger peers */

    const esp_timer_create_args_t args = {
        .callback = &srp_eval_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "srp_eval",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &s_srp_eval_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SRP fallback: timer create failed (%s)", esp_err_to_name(err));
        return err;
    }
    esp_timer_start_periodic(s_srp_eval_timer, SRP_EVAL_PERIOD_US);
    s_srp_fallback_active = true;
    ESP_LOGI(TAG, "SRP fallback controller started — yields to any Thread border router");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "SRP server not compiled in (OPENTHREAD_CONFIG_SRP_SERVER_ENABLE=0)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" esp_err_t matter_srp_fallback_pause(bool pause)
{
#if OPENTHREAD_CONFIG_SRP_SERVER_ENABLE
    if (!s_srp_fallback_active || s_srp_eval_timer == nullptr) return ESP_OK;
    if (pause == s_srp_fallback_paused) return ESP_OK;
    s_srp_fallback_paused = pause;

    if (pause) {
        esp_timer_stop(s_srp_eval_timer);
        chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
        otInstance *instance = esp_openthread_get_instance();
        if (instance) srp_set_enabled_locked(instance, false);
        chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();
        s_srp_no_br_ticks = 0;
        s_srp_yield_ticks = 0;
        srp_pick_enable_target();
        ESP_LOGW(TAG, "SRP fallback paused");
        return ESP_OK;
    }

    esp_err_t err = esp_timer_start_periodic(s_srp_eval_timer, SRP_EVAL_PERIOD_US);
    if (err != ESP_OK) {
        s_srp_fallback_paused = true;   /* stay paused so a retry can resume */
        ESP_LOGE(TAG, "SRP fallback: resume failed (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SRP fallback resumed — re-running the election");
    return ESP_OK;
#else
    (void)pause;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" esp_err_t matter_thread_router_eligible_set(bool eligible)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_err_t ret = ESP_OK;
    chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
    otInstance *instance = esp_openthread_get_instance();
    if (instance == nullptr) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        otError err = otThreadSetRouterEligible(instance, eligible);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otThreadSetRouterEligible(%d) failed: %d", eligible, err);
            ret = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "Thread router eligibility -> %s (role now %d)",
                     eligible ? "enabled" : "disabled",
                     (int)otThreadGetDeviceRole(instance));
        }
    }
    chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();
    return ret;
#else
    (void)eligible;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ------------------------------------------------------------------------- *
 * Thread IPv6 address logger (spike): print the device's Thread unicast
 * addresses so the management page can be reached over IPv6/Thread from a
 * browser. The globally-routable OMR address (SLAAC origin, handed out by a
 * border router) is the one to use from the LAN; link-local (fe80::) and the
 * mesh-local EID are not reachable off-mesh.
 * ------------------------------------------------------------------------- */
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
static esp_timer_handle_t s_addr_log_timer = nullptr;
static bool s_omr_logged = false;
static char s_srp_http_instance[64] = {0};
static bool s_srp_http_logged = false;

static const char *addr_origin_str(uint8_t origin)
{
    switch (origin) {
    case OT_ADDRESS_ORIGIN_THREAD: return "thread";
    case OT_ADDRESS_ORIGIN_SLAAC:  return "slaac";
    case OT_ADDRESS_ORIGIN_DHCPV6: return "dhcp6";
    case OT_ADDRESS_ORIGIN_MANUAL: return "manual";
    default:                       return "?";
    }
}

extern "C" void matter_log_thread_addrs(void)
{
    chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
    otInstance *instance = esp_openthread_get_instance();
    if (instance) {
        const otMeshLocalPrefix *ml = otThreadGetMeshLocalPrefix(instance);
        ESP_LOGW(TAG, "---- Thread IPv6 addresses (role=%d) ----",
                 (int)otThreadGetDeviceRole(instance));
        for (const otNetifAddress *a = otIp6GetUnicastAddresses(instance);
             a != nullptr; a = a->mNext) {
            char buf[OT_IP6_ADDRESS_STRING_SIZE];
            otIp6AddressToString(&a->mAddress, buf, sizeof(buf));

            const uint8_t *b = a->mAddress.mFields.m8;
            bool link_local = (b[0] == 0xfe && (b[1] & 0xc0) == 0x80);
            bool mesh_local = (ml && memcmp(b, ml->m8, 8) == 0);
            const char *hint = "";
            if (link_local)      hint = " (link-local, not routable off-mesh)";
            else if (mesh_local) hint = " (mesh-local, not routable off-mesh)";
            else if (a->mAddressOrigin == OT_ADDRESS_ORIGIN_SLAAC)
                hint = " <-- OMR, try http://[this]/ from the LAN";

            ESP_LOGW(TAG, "  %s/%u origin=%s%s",
                     buf, a->mPrefixLength, addr_origin_str(a->mAddressOrigin), hint);
        }
        ESP_LOGW(TAG, "-----------------------------------------");
    }
    chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();
}

/* True once the device owns a globally-routable OMR address (SLAAC origin),
 * i.e. a border router has handed out a prefix. Used to stop the one-shot
 * address logging once it has something useful to show. */
static bool thread_has_omr(void)
{
    bool found = false;
    chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
    otInstance *instance = esp_openthread_get_instance();
    if (instance) {
        const otMeshLocalPrefix *ml = otThreadGetMeshLocalPrefix(instance);
        for (const otNetifAddress *a = otIp6GetUnicastAddresses(instance);
             a != nullptr; a = a->mNext) {
            const uint8_t *b = a->mAddress.mFields.m8;
            bool link_local = (b[0] == 0xfe && (b[1] & 0xc0) == 0x80);
            bool mesh_local = (ml && memcmp(b, ml->m8, 8) == 0);
            if (!link_local && !mesh_local &&
                a->mAddressOrigin == OT_ADDRESS_ORIGIN_SLAAC) {
                found = true;
                break;
            }
        }
    }
    chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();
    return found;
}

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD_SRP_CLIENT
/* Runs on the Matter/CHIP task (via ScheduleWork) so it can never interleave
 * with CHIP's own SRP advertise cycle. ThreadStackMgr().AddSrpService() is the
 * same managed entry point CHIP uses for its operational service: it adds to
 * CHIP's SRP service array (deduped by instance+type, so no "RRset duplicated"
 * collision) and attaches to the SRP host CHIP already registered. CHIP's
 * advertise cycle marks all services invalid and removes the ones it did not
 * re-add, so this must be re-invoked periodically to keep _http._tcp alive; a
 * re-add of an unchanged service is a cheap no-op (just clears the invalid
 * flag, no SRP traffic). */
static void srp_add_http_service(intptr_t /*arg*/)
{
    if (s_srp_http_instance[0] == '\0') {
        snprintf(s_srp_http_instance, sizeof(s_srp_http_instance), "%s", ota_hostname_get());
    }
    chip::Span<const char * const> noSubTypes;
    chip::Span<const chip::Dnssd::TextEntry> noTxtEntries;
    CHIP_ERROR err = chip::DeviceLayer::ThreadStackMgr().AddSrpService(
        s_srp_http_instance, "_http._tcp", 80, noSubTypes, noTxtEntries);
    if (err == CHIP_NO_ERROR) {
        if (!s_srp_http_logged) {
            ESP_LOGW(TAG, "SRP: advertising _http._tcp '%s' (port 80) via CHIP SRP client "
                          "-- discover with 'dns-sd -B _http._tcp' / avahi-browse; a border "
                          "router advertising proxy publishes it as LAN mDNS",
                     s_srp_http_instance);
            s_srp_http_logged = true;
        }
    }
    /* On error the SRP host is not set up yet (Thread not attached) -- the next
     * timer tick retries. */
}

extern "C" void matter_srp_advertise_httpd(void)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(srp_add_http_service, 0);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "SRP: ScheduleWork failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
}
#else
extern "C" void matter_srp_advertise_httpd(void) {}
#endif

extern "C" void matter_thread_addr_log_start(void)
{
    if (s_addr_log_timer) return;
    const esp_timer_create_args_t args = {
        .callback = [](void *) {
            /* Log the IPv6 addresses until an OMR address shows up, then go
             * quiet -- one useful dump instead of every 15 s forever. */
            if (!s_omr_logged) {
                matter_log_thread_addrs();
                if (thread_has_omr()) s_omr_logged = true;
            }
            /* Keep (re-)publishing _http._tcp so it survives CHIP's advertise
             * cycles; cheap no-op once registered. */
            matter_srp_advertise_httpd();
        },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "addr_log",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_addr_log_timer) == ESP_OK) {
        esp_timer_start_periodic(s_addr_log_timer, 15 * 1000 * 1000);  /* 15 s */
        ESP_LOGI(TAG, "Thread IPv6 address logger started");
    }
    matter_log_thread_addrs();     /* also log immediately */
    matter_srp_advertise_httpd();  /* register _http._tcp once the SRP host exists */
}
#else
extern "C" void matter_log_thread_addrs(void) {}
extern "C" void matter_thread_addr_log_start(void) {}
extern "C" void matter_srp_advertise_httpd(void) {}
#endif

extern "C" void matter_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset requested");
    esp_matter::factory_reset();   /* wipes Matter NVS + reboot */
}

/* Commission mode: delete every commissioned fabric through the CHIP FabricTable
 * API. This removes the NOCs, operational keys and index metadata regardless of
 * which NVS namespace/partition they live in — unlike erasing the "chip-kvs"
 * namespace directly, which misses fabric data on devices whose KVS lives in a
 * separate partition. Scripts and WiFi config (in the "ota" namespace) are kept.
 * After this the device boots uncommissioned → BLE commissioning advertising. */
extern "C" void matter_delete_all_fabrics(void)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto &fabricTable = chip::Server::GetInstance().GetFabricTable();

    /* Collect indices first — deleting while iterating invalidates the iterator. */
    chip::FabricIndex indices[CHIP_CONFIG_MAX_FABRICS];
    size_t count = 0;
    for (const auto &fb : fabricTable) {
        if (count < CHIP_CONFIG_MAX_FABRICS) {
            indices[count++] = fb.GetFabricIndex();
        }
    }
    for (size_t i = 0; i < count; i++) {
        CHIP_ERROR err = fabricTable.Delete(indices[i]);
        ESP_LOGW(TAG, "Deleted fabric index %u: %s", indices[i], chip::ErrorStr(err));
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    ESP_LOGW(TAG, "matter_delete_all_fabrics: removed %u fabric(s)", (unsigned)count);
}

extern "C" int matter_binding_dump(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return 0;
    size_t off = 0;
    int count = 0;

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    for (const auto &e : Binding::Table::GetInstance()) {
        count++;
        int n = snprintf(buf + off, buf_len - off,
            "%s[%d] type=%u local_ep=%u remote_ep=%u node=0x%llx group=%u cluster=0x%lx fabric=%u",
            (off > 0 ? "\n" : ""), count,
            (unsigned) e.type, e.local, e.remote,
            (unsigned long long) e.nodeId, (unsigned) e.groupId,
            (unsigned long) e.clusterId.value_or(0), (unsigned) e.fabricIndex);
        if (n < 0 || (size_t) n >= buf_len - off) { off = buf_len - 1; break; }
        off += n;
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    buf[off] = '\0';
    return count;
}

extern "C" uint16_t matter_get_slot_endpoint(uint8_t slot)
{
    if (slot >= s_num_slots) return 0;
    return s_slot_endpoints[slot];
}


/* ---------------- Endpoint setup ---------------- */

static esp_err_t identify_cb(identification::callback_type_t /*type*/, uint16_t endpoint_id,
                             uint8_t /*effect_id*/, uint8_t /*effect_variant*/, void * /*priv*/)
{
    ESP_LOGI(TAG, "Identify on ep=%u", endpoint_id);
    return ESP_OK;
}

static esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                     uint32_t cluster_id, uint32_t attribute_id,
                                     esp_matter_attr_val_t *val, void * /*priv*/)
{
    if (type != PRE_UPDATE) return ESP_OK;
    if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id)
        return ESP_OK;

    /* Check if this endpoint is a relay slot (RELAY -> ch0, RELAY2 -> ch1) */
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_endpoints[i] != endpoint_id) continue;
        int ch = -1;
        if (s_slot_types[i] == SLOT_TYPE_RELAY)  ch = 0;
        if (s_slot_types[i] == SLOT_TYPE_RELAY2) ch = 1;
        if (ch >= 0) {
            relay_set_ch(ch, val->val.b);
            ESP_LOGI(TAG, "EP%u OnOff -> relay ch%d %s", endpoint_id, ch,
                     val->val.b ? "ON" : "OFF");
            break;
        }
    }
    return ESP_OK;
}

/* BindingManager: esp_matter v1.5 defers initialization until kDnssdInitialized.
 * Without a border router / SRP server, DNS-SD never becomes ready and the
 * binding table is never loaded from persistent storage.  We force-init here
 * so bindings work even without network connectivity. */
static void force_binding_manager_init(intptr_t)
{
    esp_matter::client::binding_manager_init();
    ESP_LOGI(TAG, "BindingManager force-initialized (table loaded from NVS)");
}

static endpoint_t *create_endpoint_for_type(node_t *node, script_slot_type_t type)
{
    switch (type) {
    case SLOT_TYPE_ONOFF_TOGGLE:
    case SLOT_TYPE_DIMMER: {
        /* OnOff Light Switch + LevelControl + ColorControl + Binding */
        on_off_light_switch::config_t cfg;
        endpoint_t *ep = on_off_light_switch::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
        level_control::config_t lvl_cfg;
        level_control::create(ep, &lvl_cfg, CLUSTER_FLAG_CLIENT);
        color_control::config_t cc_cfg;
        color_control::create(ep, &cc_cfg, CLUSTER_FLAG_CLIENT);
        binding::config_t bind_cfg;
        binding::create(ep, &bind_cfg, CLUSTER_FLAG_SERVER);
        return ep;
    }
    case SLOT_TYPE_ONOFF_STATE: {
        /* OnOff Light Switch + Binding (state-follow) */
        on_off_light_switch::config_t cfg;
        endpoint_t *ep = on_off_light_switch::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
        binding::config_t bind_cfg;
        binding::create(ep, &bind_cfg, CLUSTER_FLAG_SERVER);
        return ep;
    }
    case SLOT_TYPE_TEMPERATURE: {
        /* Build the Temperature Sensor as a plain ember-stored cluster (same as
         * the Contact Sensor). The temperature_sensor::create() helper registers
         * MeasuredValue with external/managed storage, so attribute::update()
         * never writes it and the value stays null in the data model. Creating
         * the cluster generically keeps MeasuredValue writable via the esp-matter
         * attribute store. MeasuredValue/Min/Max are nullable int16. */
        endpoint_t *ep = esp_matter::endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
        if (!ep) return NULL;
        descriptor::config_t desc_cfg;
        descriptor::create(ep, &desc_cfg, CLUSTER_FLAG_SERVER);
        esp_matter::endpoint::add_device_type(ep, 0x0302 /* Temperature Sensor */, 2);
        cluster_t *cl = esp_matter::cluster::create(ep, TemperatureMeasurement::Id, CLUSTER_FLAG_SERVER);
        global::attribute::create_cluster_revision(cl, 1);
        global::attribute::create_feature_map(cl, 0);
        esp_matter::attribute::create(cl, TemperatureMeasurement::Attributes::MeasuredValue::Id,
                                      ATTRIBUTE_FLAG_NULLABLE, esp_matter_nullable_int16(nullable<int16_t>()));
        esp_matter::attribute::create(cl, TemperatureMeasurement::Attributes::MinMeasuredValue::Id,
                                      ATTRIBUTE_FLAG_NULLABLE, esp_matter_nullable_int16(nullable<int16_t>(-4000)));
        esp_matter::attribute::create(cl, TemperatureMeasurement::Attributes::MaxMeasuredValue::Id,
                                      ATTRIBUTE_FLAG_NULLABLE, esp_matter_nullable_int16(nullable<int16_t>(12500)));
        return ep;
    }
    case SLOT_TYPE_OCCUPANCY: {
        /* Build the Occupancy Sensor as a plain ember-stored cluster (same as the
         * Contact/Temperature sensors). The occupancy_sensor::create() helper
         * registers Occupancy with external/managed storage, so attribute::update()
         * never writes it and it stays 0 in the data model. Occupancy and the two
         * SensorType attributes are mandatory; PIR (type 0, bitmap bit0). */
        endpoint_t *ep = esp_matter::endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
        if (!ep) return NULL;
        descriptor::config_t desc_cfg;
        descriptor::create(ep, &desc_cfg, CLUSTER_FLAG_SERVER);
        esp_matter::endpoint::add_device_type(ep, 0x0107 /* Occupancy Sensor */, 4);
        cluster_t *cl = esp_matter::cluster::create(ep, OccupancySensing::Id, CLUSTER_FLAG_SERVER);
        global::attribute::create_cluster_revision(cl, 5);
        global::attribute::create_feature_map(cl, 1 /* PIR */);
        esp_matter::attribute::create(cl, OccupancySensing::Attributes::Occupancy::Id,
                                      ATTRIBUTE_FLAG_NONE, esp_matter_bitmap8(0));
        esp_matter::attribute::create(cl, OccupancySensing::Attributes::OccupancySensorType::Id,
                                      ATTRIBUTE_FLAG_NONE, esp_matter_enum8(0 /* PIR */));
        esp_matter::attribute::create(cl, OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id,
                                      ATTRIBUTE_FLAG_NONE, esp_matter_bitmap8(1 /* PIR */));
        return ep;
    }
    case SLOT_TYPE_RELAY: {
        on_off_light::config_t cfg;
        cfg.on_off.on_off = relay_get_ch(0);
        return on_off_light::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
    }
    case SLOT_TYPE_RELAY2: {
        on_off_light::config_t cfg;
        cfg.on_off.on_off = relay_get_ch(1);
        return on_off_light::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
    }
    case SLOT_TYPE_CONTACT: {
        /* Build the Contact Sensor the same way as Occupancy: a plain ember-stored
         * BooleanState cluster. The contact_sensor::create() helper registers a
         * code-driven BooleanState server whose StateValue is "managed internally",
         * which makes attribute::update() fail with ESP_ERR_NOT_SUPPORTED (262).
         * Creating the cluster generically keeps StateValue writable via the
         * esp-matter attribute store. */
        endpoint_t *ep = esp_matter::endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
        if (!ep) return NULL;
        descriptor::config_t desc_cfg;
        descriptor::create(ep, &desc_cfg, CLUSTER_FLAG_SERVER);
        esp_matter::endpoint::add_device_type(ep, 0x0015 /* Contact Sensor */, 1);
        cluster_t *cl = esp_matter::cluster::create(ep, BooleanState::Id, CLUSTER_FLAG_SERVER);
        global::attribute::create_cluster_revision(cl, 1);
        global::attribute::create_feature_map(cl, 0);
        esp_matter::attribute::create(cl, BooleanState::Attributes::StateValue::Id,
                                      ATTRIBUTE_FLAG_NONE, esp_matter_bool(false));
        return ep;
    }
    case SLOT_TYPE_ILLUMINANCE: {
        /* Plain ember-stored IlluminanceMeasurement cluster (same approach as the
         * Contact/Temperature sensors) so MeasuredValue is writable from Lua via
         * attribute::update(). MeasuredValue/Min/Max are nullable uint16; the
         * value is the Matter-encoded lux (10000*log10(lux)+1). */
        endpoint_t *ep = esp_matter::endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
        if (!ep) return NULL;
        descriptor::config_t desc_cfg;
        descriptor::create(ep, &desc_cfg, CLUSTER_FLAG_SERVER);
        esp_matter::endpoint::add_device_type(ep, 0x0106 /* Light Sensor */, 2);
        cluster_t *cl = esp_matter::cluster::create(ep, IlluminanceMeasurement::Id, CLUSTER_FLAG_SERVER);
        global::attribute::create_cluster_revision(cl, 3);
        global::attribute::create_feature_map(cl, 0);
        esp_matter::attribute::create(cl, IlluminanceMeasurement::Attributes::MeasuredValue::Id,
                                      ATTRIBUTE_FLAG_NULLABLE, esp_matter_nullable_uint16(nullable<uint16_t>()));
        esp_matter::attribute::create(cl, IlluminanceMeasurement::Attributes::MinMeasuredValue::Id,
                                      ATTRIBUTE_FLAG_NULLABLE, esp_matter_nullable_uint16(nullable<uint16_t>(1)));
        esp_matter::attribute::create(cl, IlluminanceMeasurement::Attributes::MaxMeasuredValue::Id,
                                      ATTRIBUTE_FLAG_NULLABLE, esp_matter_nullable_uint16(nullable<uint16_t>(0xFFFE)));
        return ep;
    }
    case SLOT_TYPE_POWER: {
        /* Electrical Sensor device with an ElectricalPowerMeasurement cluster,
         * driven from Lua via matter_update_power_ep(). electrical_sensor::create
         * sets up descriptor + power_topology + the mandatory ActivePower
         * attribute; the optional Voltage/ActiveCurrent/Frequency attributes are
         * added here so Lua can report them too (all nullable int64, mW/mV/mA/mHz). */
        electrical_sensor::config_t pm_cfg;
        endpoint_t *ep = electrical_sensor::create(node, &pm_cfg, ENDPOINT_FLAG_NONE, NULL);
        if (!ep) return NULL;
        cluster_t *epm = cluster::get(ep, ElectricalPowerMeasurement::Id);
        if (epm) {
            electrical_power_measurement::attribute::create_voltage(epm, nullable<int64_t>());
            electrical_power_measurement::attribute::create_active_current(epm, nullable<int64_t>());
            electrical_power_measurement::attribute::create_frequency(epm, nullable<int64_t>());
        }
        return ep;
    }
    default:
        return NULL;
    }
}

static const char *slot_type_name(script_slot_type_t type)
{
    switch (type) {
    case SLOT_TYPE_ONOFF_TOGGLE: return "OnOff Toggle+Dim+Color (client)";
    case SLOT_TYPE_DIMMER:       return "OnOff Toggle+Dim+Color (client)";
    case SLOT_TYPE_ONOFF_STATE:  return "OnOff State-Follow (client)";
    case SLOT_TYPE_TEMPERATURE:  return "Temperature Sensor";
    case SLOT_TYPE_OCCUPANCY:    return "Occupancy Sensor";
    case SLOT_TYPE_RELAY:        return "OnOff Light (relay 1)";
    case SLOT_TYPE_RELAY2:       return "OnOff Light (relay 2)";
    case SLOT_TYPE_CONTACT:      return "Contact Sensor (BooleanState)";
    case SLOT_TYPE_ILLUMINANCE:  return "Illuminance Sensor";
    case SLOT_TYPE_POWER:        return "Electrical Power Measurement";
    default:                     return "Unknown";
    }
}

extern "C" esp_err_t matter_start(const script_slot_type_t *slot_types, uint8_t num_slots)
{
    /* Store slot types for later use by update functions */
    s_num_slots = (num_slots > SCRIPT_MAX_SLOTS) ? SCRIPT_MAX_SLOTS : num_slots;
    for (int i = 0; i < s_num_slots; i++) {
        s_slot_types[i] = slot_types[i];
        s_slot_endpoints[i] = 0;
    }

    /* Node (root) */
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, attribute_update_cb, identify_cb);
    if (!node) { ESP_LOGE(TAG, "node create failed"); return ESP_FAIL; }

    /* Create endpoints dynamically based on slot configuration */
    for (int i = 0; i < s_num_slots; i++) {
        if (slot_types[i] == SLOT_TYPE_NONE) continue;

        endpoint_t *ep = create_endpoint_for_type(node, slot_types[i]);
        if (ep) {
            s_slot_endpoints[i] = endpoint::get_id(ep);
            ESP_LOGI(TAG, "Slot %d: EP%u = %s", i, s_slot_endpoints[i],
                     slot_type_name(slot_types[i]));
        } else {
            ESP_LOGW(TAG, "Slot %d: failed to create endpoint for type %d", i, slot_types[i]);
        }
    }

    /* Electrical Power Measurement endpoint(s) — on PM hardware.
     * 1PM Gen4 (BL0942): one channel. 2PM Gen4 (ADE7953): two channels.
     * electrical_sensor::create sets up descriptor + power_topology +
     * electrical_power_measurement (with the mandatory ActivePower attribute).
     * The optional Voltage/ActiveCurrent/Frequency attributes are added here so
     * matter_update_power_ch() can report them. */
    if (hw_profile()->has_pm) {
        int pm_channels = (hw_profile()->pm_type == PM_ADE7953) ? 2 : 1;
        for (int ch = 0; ch < pm_channels; ch++) {
            electrical_sensor::config_t pm_cfg;
            endpoint_t *pm_ep = electrical_sensor::create(node, &pm_cfg, ENDPOINT_FLAG_NONE, NULL);
            if (pm_ep) {
                cluster_t *epm = cluster::get(pm_ep, ElectricalPowerMeasurement::Id);
                if (epm) {
                    electrical_power_measurement::attribute::create_voltage(epm, nullable<int64_t>());
                    electrical_power_measurement::attribute::create_active_current(epm, nullable<int64_t>());
                    electrical_power_measurement::attribute::create_frequency(epm, nullable<int64_t>());
                }
                s_pm_endpoint[ch] = endpoint::get_id(pm_ep);
                ESP_LOGI(TAG, "EP%u = Electrical Power Measurement (ch%d)",
                         s_pm_endpoint[ch], ch);
            } else {
                ESP_LOGW(TAG, "failed to create electrical sensor endpoint (ch%d)", ch);
            }
        }
    }

    /* OTA cluster requestor */
    esp_matter_ota_requestor_init();

    /* The Matter OTA requestor applies images with esp_ota_set_boot_partition(),
     * which writes the ESP-IDF otadata format and thereby overwrites the stock
     * loader's SH0S boot-select. Once the image is applied (but before the
     * requestor reboots) mirror the new slot back into a valid SH0S so the
     * stock loader actually boots the freshly downloaded image. */
    CHIP_ERROR cerr = chip::DeviceLayer::PlatformMgr().AddEventHandler(shelly_ota_apply_handler, 0);
    if (cerr != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "OTA apply handler not registered: %" CHIP_ERROR_FORMAT, cerr.Format());
    }

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    cerr = chip::DeviceLayer::PlatformMgr().AddEventHandler(thread_netdata_handler, 0);
    if (cerr != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Thread network-data handler not registered: %" CHIP_ERROR_FORMAT,
                 cerr.Format());
    }
#endif

    keepalive_init();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_cfg = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_cfg);
#endif

    /* Start the stack */
    esp_err_t err = esp_matter::start(NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_matter::start: %d", err); return err; }

    /* Force-init BindingManager so binding table loads from NVS immediately,
     * regardless of DNS-SD / network state.  Must run on CHIP thread. */
    cerr = chip::DeviceLayer::PlatformMgr().ScheduleWork(force_binding_manager_init, 0);
    if (cerr != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "BindingManager init not scheduled: %" CHIP_ERROR_FORMAT, cerr.Format());
    }

    return ESP_OK;
}
