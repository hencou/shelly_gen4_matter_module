/*
 * Lua scripting engine for the Shelly Matter Platform.
 *
 * Each slot has:
 *   - A configuration (type, trigger, script source)
 *   - A Lua state (compiled script)
 *   - An associated Matter endpoint (created dynamically)
 *
 * Scripts are executed by a dedicated FreeRTOS task that:
 *   - Periodically runs TRIGGER_PERIODIC scripts
 *   - Runs TRIGGER_INPUT_CHANGE scripts on input state changes
 *   - Runs TRIGGER_BUTTON_EVENT scripts when button events arrive
 */

#include "script_engine.h"
#include "app_config.h"
#include "hw_config.h"
#include "relay.h"
#include "sensors.h"
#include "matter_device.h"
#include "chip_temp.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "script";

#define NVS_NS_SCRIPT    "scripts"
#define SCRIPT_TASK_STACK 6144
#define EVENT_QUEUE_LEN   16

/* ---------- Runtime state per slot ---------- */

typedef struct {
    script_slot_config_t cfg;
    lua_State *L;
    uint16_t endpoint_id;
    bool active;
    TickType_t last_run;
} script_slot_t;

static script_slot_t s_slots[SCRIPT_MAX_SLOTS];
static TaskHandle_t s_task = NULL;
static QueueHandle_t s_event_queue = NULL;

/* Shared input state (updated from ISR/sensor callbacks) */
static volatile int16_t s_temperature_centi = 0;
static volatile uint8_t s_analog_percent = 0;
static volatile bool s_digital_in = false;
static volatile bool s_sw_state = false;
static volatile bool s_device_btn = false;

/* Last button event (consumed by scripts) */
typedef struct {
    input_id_t input;
    button_event_t event;
} script_event_t;

static volatile script_event_t s_last_btn_event = {0, 0};
static volatile bool s_btn_event_pending = false;

/* ---------- NVS helpers ---------- */

static void slot_nvs_key(uint8_t slot, const char *suffix, char *buf, size_t len)
{
    snprintf(buf, len, "s%u_%s", slot, suffix);
}

static esp_err_t slot_save(uint8_t slot, const script_slot_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_SCRIPT, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    char key[16];
    slot_nvs_key(slot, "type", key, sizeof(key));
    nvs_set_u8(h, key, (uint8_t)cfg->type);

    slot_nvs_key(slot, "trig", key, sizeof(key));
    nvs_set_u8(h, key, (uint8_t)cfg->trigger);

    slot_nvs_key(slot, "per", key, sizeof(key));
    nvs_set_u16(h, key, cfg->period_ms);

    slot_nvs_key(slot, "name", key, sizeof(key));
    nvs_set_str(h, key, cfg->name);

    slot_nvs_key(slot, "code", key, sizeof(key));
    nvs_set_str(h, key, cfg->script);

    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t slot_load(uint8_t slot, script_slot_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_SCRIPT, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    memset(cfg, 0, sizeof(*cfg));

    char key[16];
    uint8_t u8val;

    slot_nvs_key(slot, "type", key, sizeof(key));
    if (nvs_get_u8(h, key, &u8val) == ESP_OK) cfg->type = (script_slot_type_t)u8val;

    slot_nvs_key(slot, "trig", key, sizeof(key));
    if (nvs_get_u8(h, key, &u8val) == ESP_OK) cfg->trigger = (script_trigger_t)u8val;

    slot_nvs_key(slot, "per", key, sizeof(key));
    nvs_get_u16(h, key, &cfg->period_ms);

    size_t len = sizeof(cfg->name);
    slot_nvs_key(slot, "name", key, sizeof(key));
    nvs_get_str(h, key, cfg->name, &len);

    len = sizeof(cfg->script);
    slot_nvs_key(slot, "code", key, sizeof(key));
    nvs_get_str(h, key, cfg->script, &len);

    nvs_close(h);
    return ESP_OK;
}

/* ---------- Lightweight NVS slot type loader (before matter_start) ---------- */

esp_err_t script_engine_load_slot_types(script_slot_type_t *types, uint8_t max_slots)
{
    for (int i = 0; i < max_slots; i++)
        types[i] = SLOT_TYPE_NONE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_SCRIPT, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK; /* no scripts namespace = all NONE */

    for (int i = 0; i < max_slots; i++) {
        char key[16];
        slot_nvs_key(i, "type", key, sizeof(key));
        uint8_t val = 0;
        if (nvs_get_u8(h, key, &val) == ESP_OK)
            types[i] = (script_slot_type_t)val;
    }
    nvs_close(h);
    return ESP_OK;
}

/* ---------- Lua API: input ---------- */

static int l_input_analog(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)s_analog_percent);
    return 1;
}

static int l_input_digital(lua_State *L)
{
    lua_pushboolean(L, s_digital_in);
    return 1;
}

static int l_input_sw(lua_State *L)
{
    lua_pushboolean(L, s_sw_state);
    return 1;
}

static int l_input_device_btn(lua_State *L)
{
    lua_pushboolean(L, s_device_btn);
    return 1;
}

static int l_input_temperature(lua_State *L)
{
    lua_pushnumber(L, s_temperature_centi / 100.0);
    return 1;
}

static int l_input_chip_temperature(lua_State *L)
{
    float t;
    if (chip_temp_read(&t)) {
        lua_pushnumber(L, t);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_input_button_event(lua_State *L)
{
    if (!s_btn_event_pending) {
        lua_pushnil(L);
        return 1;
    }
    switch (s_last_btn_event.event) {
        case BTN_EVT_SHORT_PRESS:      lua_pushstring(L, "short_press"); break;
        case BTN_EVT_LONG_PRESS_START: lua_pushstring(L, "long_press_start"); break;
        case BTN_EVT_LONG_PRESS_STOP:  lua_pushstring(L, "long_press_stop"); break;
        case BTN_EVT_DOUBLE_PRESS:     lua_pushstring(L, "double_press"); break;
        case BTN_EVT_SHORT_LONG_START: lua_pushstring(L, "short_long_start"); break;
        case BTN_EVT_SHORT_LONG_STOP:  lua_pushstring(L, "short_long_stop"); break;
        case BTN_EVT_CONTACT_CLOSED:   lua_pushstring(L, "contact_closed"); break;
        case BTN_EVT_CONTACT_OPEN:     lua_pushstring(L, "contact_open"); break;
        case BTN_EVT_MODE_TOGGLE:      lua_pushstring(L, "mode_toggle"); break;
        default:                       lua_pushnil(L); break;
    }
    return 1;
}

static int l_input_button_id(lua_State *L)
{
    lua_pushinteger(L, (int)s_last_btn_event.input);
    return 1;
}

static const luaL_Reg input_lib[] = {
    {"analog",       l_input_analog},
    {"digital",      l_input_digital},
    {"sw",           l_input_sw},
    {"device_btn",   l_input_device_btn},
    {"temperature",  l_input_temperature},
    {"chip_temperature", l_input_chip_temperature},
    {"button_event", l_input_button_event},
    {"button_id",    l_input_button_id},
    {NULL, NULL}
};

/* ---------- Lua API: output ---------- */

/* Optional leading channel argument (1-based in Lua). Returns 0-based channel
 * and sets *val_idx to the Lua stack index of the following value argument.
 *   relay_set(on)      -> ch 0, value at index 1
 *   relay_set(ch, on)  -> ch = ch-1, value at index 2 */
static int relay_arg_channel(lua_State *L, int *val_idx)
{
    if (lua_gettop(L) >= 2 && lua_isnumber(L, 1)) {
        *val_idx = 2;
        return (int)lua_tointeger(L, 1) - 1;
    }
    *val_idx = 1;
    return 0;
}

static int l_output_relay(lua_State *L)
{
    int vi;
    int ch = relay_arg_channel(L, &vi);
    bool on = lua_toboolean(L, vi);
    relay_set_ch(ch, on);
    matter_update_relay_onoff(ch, on);
    return 0;
}

static int l_output_relay_toggle(lua_State *L)
{
    int ch = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) - 1 : 0;
    relay_toggle_ch(ch);
    matter_update_relay_onoff(ch, relay_get_ch(ch));
    return 0;
}

static int l_output_relay_state(lua_State *L)
{
    int ch = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) - 1 : 0;
    lua_pushboolean(L, relay_get_ch(ch));
    return 1;
}

static const luaL_Reg output_lib[] = {
    {"relay",        l_output_relay},
    {"relay_set",    l_output_relay},
    {"relay_toggle", l_output_relay_toggle},
    {"relay_state",  l_output_relay_state},
    {NULL, NULL}
};

/* ---------- Lua API: endpoint ---------- */

/* endpoint.set(attr_name, value) — for sensor endpoints */
static int l_endpoint_set(lua_State *L)
{
    /* Slot index stored in registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "_slot_idx");
    int slot = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    const char *attr = luaL_checkstring(L, 1);

    /* A slot writes its own endpoint. The fan-out variants exist for the
     * built-in sensor callbacks, which have no slot to speak of; using them here
     * makes every slot of the same type overwrite the others, so with two
     * Temperature Sensors both report whichever script ran last. */
    if (strcmp(attr, "occupied") == 0) {
        bool val = lua_toboolean(L, 2);
        ESP_LOGI(TAG, "slot %d: set occupied=%d", slot, val);
        matter_update_occupancy_ep(s_slots[slot].endpoint_id, val);
    } else if (strcmp(attr, "measured_value") == 0) {
        int val = (int)luaL_checkinteger(L, 2);
        ESP_LOGI(TAG, "slot %d: set measured_value=%d", slot, val);
        matter_update_temperature_ep(s_slots[slot].endpoint_id, (int16_t)val);
    } else if (strcmp(attr, "illuminance") == 0) {
        double lux = (double)luaL_checknumber(L, 2);
        ESP_LOGI(TAG, "slot %d: set illuminance=%.1f lux", slot, lux);
        matter_update_illuminance_ep(s_slots[slot].endpoint_id, (float)lux);
    } else if (strcmp(attr, "on_off") == 0) {
        bool val = lua_toboolean(L, 2);
        ESP_LOGI(TAG, "slot %d: set on_off=%d", slot, val);
        matter_update_relay_onoff(0, val);
    } else if (strcmp(attr, "state") == 0) {
        bool val = lua_toboolean(L, 2);
        uint16_t ep = s_slots[slot].endpoint_id;
        ESP_LOGI(TAG, "slot %d: set state=%d ep=%u", slot, val, ep);
        matter_update_boolean_state(ep, val);
    } else if (strcmp(attr, "power") == 0 || strcmp(attr, "active_power") == 0) {
        /* Electrical Power Measurement. Scalar arg sets ActivePower (W) only;
         * table arg {power=, voltage=, current=, frequency=} sets any subset.
         * NAN = leave that attribute unchanged. Units: W / V / A / Hz. */
        float p = NAN, v = NAN, c = NAN, f = NAN;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "power");     if (!lua_isnil(L, -1)) p = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 2, "voltage");   if (!lua_isnil(L, -1)) v = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 2, "current");   if (!lua_isnil(L, -1)) c = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 2, "frequency"); if (!lua_isnil(L, -1)) f = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        } else {
            p = (float)luaL_checknumber(L, 2);
        }
        uint16_t ep = s_slots[slot].endpoint_id;
        ESP_LOGI(TAG, "slot %d: set power=%.1fW ep=%u", slot, p, ep);
        matter_update_power_ep(ep, v, c, p, f);
    } else {
        ESP_LOGW(TAG, "slot %d: unknown attr '%s'", slot, attr);
    }
    return 0;
}

/* endpoint.command(cmd, params_table) — for client endpoints */
static int l_endpoint_command(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "_slot_idx");
    int slot = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    const char *cmd = luaL_checkstring(L, 1);
    uint16_t ep = s_slots[slot].endpoint_id;

    if (strcmp(cmd, "toggle") == 0) {
        ESP_LOGI(TAG, "slot %d: command toggle ep=%u", slot, ep);
        matter_send_onoff_toggle(ep);
    } else if (strcmp(cmd, "on") == 0) {
        ESP_LOGI(TAG, "slot %d: command on ep=%u", slot, ep);
        matter_send_onoff_on(ep);
    } else if (strcmp(cmd, "off") == 0) {
        ESP_LOGI(TAG, "slot %d: command off ep=%u", slot, ep);
        matter_send_onoff_off(ep);
    } else if (strcmp(cmd, "move_with_onoff") == 0) {
        bool up = true;
        uint8_t rate = 50;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "up");
            if (!lua_isnil(L, -1)) up = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 2, "rate");
            if (!lua_isnil(L, -1)) rate = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        ESP_LOGI(TAG, "slot %d: command move_with_onoff up=%d rate=%u ep=%u", slot, up, rate, ep);
        matter_send_level_move(ep, up, rate);
    } else if (strcmp(cmd, "move_to_level") == 0) {
        uint8_t level = 254;
        uint16_t transition = 0;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "level");
            if (!lua_isnil(L, -1)) level = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 2, "transition");
            if (!lua_isnil(L, -1)) transition = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        ESP_LOGI(TAG, "slot %d: command move_to_level level=%u transition=%u ep=%u", slot, level, transition, ep);
        matter_send_level_move_to_level(ep, level, transition);
    } else if (strcmp(cmd, "stop") == 0) {
        ESP_LOGI(TAG, "slot %d: command stop ep=%u", slot, ep);
        matter_send_level_stop(ep);
    } else if (strcmp(cmd, "color_temp_set") == 0) {
        uint16_t kelvin = 2700;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "kelvin");
            if (!lua_isnil(L, -1)) kelvin = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        uint16_t mireds = (uint16_t)(1000000 / kelvin);
        ESP_LOGI(TAG, "slot %d: command color_temp_set kelvin=%u mireds=%u ep=%u", slot, kelvin, mireds, ep);
        matter_send_color_temp_set(ep, mireds);
    } else if (strcmp(cmd, "color_temp_move") == 0) {
        bool warmer = true;
        uint16_t rate = 50;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "warmer");
            if (!lua_isnil(L, -1)) warmer = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 2, "rate");
            if (!lua_isnil(L, -1)) rate = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        ESP_LOGI(TAG, "slot %d: command color_temp_move warmer=%d rate=%u ep=%u", slot, warmer, rate, ep);
        matter_send_color_temp_move(ep, warmer, rate);
    } else if (strcmp(cmd, "color_temp_stop") == 0) {
        ESP_LOGI(TAG, "slot %d: command color_temp_stop ep=%u", slot, ep);
        matter_send_color_temp_stop(ep);
    } else {
        ESP_LOGW(TAG, "slot %d: unknown command '%s'", slot, cmd);
    }
    return 0;
}

static const luaL_Reg endpoint_lib[] = {
    {"set",     l_endpoint_set},
    {"command", l_endpoint_command},
    {NULL, NULL}
};

/* ---------- Lua API: log ---------- */

static int l_log(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGI(TAG, "[lua] %s", msg);
    return 0;
}

/* ---------- Lua API: timer ---------- */

static int l_timer_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static const luaL_Reg timer_lib[] = {
    {"millis", l_timer_millis},
    {NULL, NULL}
};

/* ---------- Lua state setup ---------- */

static lua_State *create_lua_state(uint8_t slot)
{
    lua_State *L = luaL_newstate();
    if (!L) {
        ESP_LOGE(TAG, "slot %u: failed to create Lua state", slot);
        return NULL;
    }

    /* Open safe standard libraries (no io, os, loadlib) */
    luaL_requiref(L, "base", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);

    /* Register custom libraries */
    luaL_newlib(L, input_lib);    lua_setglobal(L, "input");
    luaL_newlib(L, output_lib);   lua_setglobal(L, "output");
    luaL_newlib(L, endpoint_lib); lua_setglobal(L, "endpoint");
    luaL_newlib(L, timer_lib);    lua_setglobal(L, "timer");

    /* Global log() function */
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "log");

    /* Store slot index in registry for API callbacks */
    lua_pushinteger(L, slot);
    lua_setfield(L, LUA_REGISTRYINDEX, "_slot_idx");

    return L;
}

static bool compile_and_load(script_slot_t *slot, uint8_t idx)
{
    if (!slot->L) return false;
    if (strlen(slot->cfg.script) == 0) return false;

    int err = luaL_loadstring(slot->L, slot->cfg.script);
    if (err != LUA_OK) {
        ESP_LOGE(TAG, "slot %u compile error: %s", idx,
                 lua_tostring(slot->L, -1));
        lua_pop(slot->L, 1);
        return false;
    }
    /* Run once to define functions / set up state */
    err = lua_pcall(slot->L, 0, 0, 0);
    if (err != LUA_OK) {
        ESP_LOGE(TAG, "slot %u init error: %s", idx,
                 lua_tostring(slot->L, -1));
        lua_pop(slot->L, 1);
        return false;
    }
    return true;
}

/* ---------- Script execution ---------- */

static void run_slot_script(script_slot_t *slot, uint8_t idx)
{
    if (!slot->active || !slot->L) return;
    if (strlen(slot->cfg.script) == 0) return;

    /* Push the "run" function if it exists, otherwise re-execute entire script */
    lua_getglobal(slot->L, "run");
    if (lua_isfunction(slot->L, -1)) {
        int err = lua_pcall(slot->L, 0, 0, 0);
        if (err != LUA_OK) {
            ESP_LOGW(TAG, "slot %u run() error: %s", idx,
                     lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    } else {
        lua_pop(slot->L, 1);
        /* No run() function — re-execute entire script */
        if (luaL_dostring(slot->L, slot->cfg.script) != LUA_OK) {
            ESP_LOGW(TAG, "slot %u exec error: %s", idx,
                     lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
}

/* ---------- Script engine task ---------- */

static void process_button_event(const script_event_t *evt)
{
    s_last_btn_event = *evt;
    s_btn_event_pending = true;

    /* Maintain the onboard button level (state-following) so that
     * input.device_btn() reflects pressed/released. The driver emits
     * CONTACT_CLOSED on every press edge and CONTACT_OPEN on release. */
    if (evt->input == INPUT_DEVICE_BTN) {
        if (evt->event == BTN_EVT_CONTACT_CLOSED)      s_device_btn = true;
        else if (evt->event == BTN_EVT_CONTACT_OPEN)   s_device_btn = false;
    }

    for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
        if (s_slots[i].active && s_slots[i].cfg.trigger == TRIGGER_BUTTON_EVENT) {
            run_slot_script(&s_slots[i], i);
        }
    }
    s_btn_event_pending = false;
}

static void script_engine_task(void *arg)
{
    (void)arg;

    while (1) {
        /* Calculate sleep: time until next periodic script is due (max 500ms) */
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = pdMS_TO_TICKS(500);
        for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
            if (!s_slots[i].active) continue;
            if (s_slots[i].cfg.trigger != TRIGGER_PERIODIC) continue;
            if (s_slots[i].cfg.period_ms == 0) continue;
            TickType_t period = pdMS_TO_TICKS(s_slots[i].cfg.period_ms);
            TickType_t elapsed = now - s_slots[i].last_run;
            TickType_t remaining = (elapsed >= period) ? 0 : (period - elapsed);
            if (remaining < wait) wait = remaining;
        }
        if (wait < pdMS_TO_TICKS(10)) wait = pdMS_TO_TICKS(10);

        /* Block on event queue — wakes instantly on button event */
        script_event_t evt;
        if (xQueueReceive(s_event_queue, &evt, wait) == pdTRUE) {
            process_button_event(&evt);
            /* Drain any additional queued events */
            while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
                process_button_event(&evt);
            }
        }

        /* Run periodic scripts — only when their interval has elapsed */
        now = xTaskGetTickCount();
        for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
            if (!s_slots[i].active) continue;
            if (s_slots[i].cfg.trigger != TRIGGER_PERIODIC) continue;
            if (s_slots[i].cfg.period_ms == 0) continue;
            TickType_t elapsed = now - s_slots[i].last_run;
            if (elapsed >= pdMS_TO_TICKS(s_slots[i].cfg.period_ms)) {
                run_slot_script(&s_slots[i], i);
                s_slots[i].last_run = xTaskGetTickCount();
            }
        }

        /* Update input state (analog, digital) */
        const hw_profile_t *hw = hw_profile();
        if (hw->has_addon && hw->addon_digital_gpio >= 0) {
            s_digital_in = gpio_get_level(hw->addon_digital_gpio);
        }
        s_sw_state = gpio_get_level(hw->switch_gpio);
    }
}

/* ---------- Public API ---------- */

esp_err_t script_engine_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_event_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(script_event_t));
    if (!s_event_queue) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "script engine initialized");
    return ESP_OK;
}

esp_err_t script_engine_start(void)
{
    /* Load all slots from NVS */
    int active_count = 0;
    for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
        if (slot_load(i, &s_slots[i].cfg) == ESP_OK && s_slots[i].cfg.type != SLOT_TYPE_NONE) {
            /* Get dynamically assigned endpoint ID from matter_device */
            s_slots[i].endpoint_id = matter_get_slot_endpoint(i);

            s_slots[i].L = create_lua_state(i);
            if (s_slots[i].L && compile_and_load(&s_slots[i], i)) {
                s_slots[i].active = true;
                active_count++;
                ESP_LOGI(TAG, "slot %d: '%s' type=%d trigger=%d ep=%u active",
                         i, s_slots[i].cfg.name, s_slots[i].cfg.type,
                         s_slots[i].cfg.trigger, s_slots[i].endpoint_id);
            } else {
                ESP_LOGW(TAG, "slot %d: failed to compile script", i);
            }
        }
    }

    if (active_count > 0 || true /* always start task for event handling */) {
        BaseType_t ret = xTaskCreate(script_engine_task, "scripts",
                                     SCRIPT_TASK_STACK, NULL, 4, &s_task);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "failed to create script task");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "script engine started: %d active slots", active_count);
    return ESP_OK;
}

esp_err_t script_slot_get(uint8_t slot, script_slot_config_t *cfg)
{
    if (slot >= SCRIPT_MAX_SLOTS) return ESP_ERR_INVALID_ARG;
    memcpy(cfg, &s_slots[slot].cfg, sizeof(*cfg));
    return ESP_OK;
}

esp_err_t script_slot_set(uint8_t slot, const script_slot_config_t *cfg)
{
    if (slot >= SCRIPT_MAX_SLOTS) return ESP_ERR_INVALID_ARG;

    /* Stop existing script */
    if (s_slots[slot].L) {
        lua_close(s_slots[slot].L);
        s_slots[slot].L = NULL;
    }
    s_slots[slot].active = false;

    /* Save config */
    memcpy(&s_slots[slot].cfg, cfg, sizeof(*cfg));
    esp_err_t err = slot_save(slot, cfg);
    if (err != ESP_OK) return err;

    /* Start new script if type != NONE */
    if (cfg->type != SLOT_TYPE_NONE && strlen(cfg->script) > 0) {
        s_slots[slot].L = create_lua_state(slot);
        if (s_slots[slot].L && compile_and_load(&s_slots[slot], slot)) {
            s_slots[slot].active = true;
            ESP_LOGI(TAG, "slot %u: script updated and running", slot);
        } else {
            ESP_LOGW(TAG, "slot %u: script compile failed", slot);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

esp_err_t script_slot_clear(uint8_t slot)
{
    if (slot >= SCRIPT_MAX_SLOTS) return ESP_ERR_INVALID_ARG;

    if (s_slots[slot].L) {
        lua_close(s_slots[slot].L);
        s_slots[slot].L = NULL;
    }
    s_slots[slot].active = false;
    memset(&s_slots[slot].cfg, 0, sizeof(s_slots[slot].cfg));

    /* Erase from NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NS_SCRIPT, NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        slot_nvs_key(slot, "type", key, sizeof(key));
        nvs_erase_key(h, key);
        slot_nvs_key(slot, "trig", key, sizeof(key));
        nvs_erase_key(h, key);
        slot_nvs_key(slot, "per", key, sizeof(key));
        nvs_erase_key(h, key);
        slot_nvs_key(slot, "name", key, sizeof(key));
        nvs_erase_key(h, key);
        slot_nvs_key(slot, "code", key, sizeof(key));
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "slot %u: cleared", slot);
    return ESP_OK;
}

void script_engine_button_event(input_id_t input, button_event_t evt)
{
    if (!s_event_queue) return;
    script_event_t e = { .input = input, .event = evt };
    xQueueSend(s_event_queue, &e, 0);
}

void script_engine_temperature_update(int16_t centi_c)
{
    s_temperature_centi = centi_c;
}

void script_engine_occupancy_update(bool occupied)
{
    (void)occupied;
}

void script_engine_analog_update(uint8_t duty_pct)
{
    s_analog_percent = duty_pct;
}

uint16_t script_engine_get_endpoint(uint8_t slot)
{
    if (slot >= SCRIPT_MAX_SLOTS) return 0;
    return s_slots[slot].endpoint_id;
}

uint8_t script_engine_active_slots(void)
{
    uint8_t count = 0;
    for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
        if (s_slots[i].active) count++;
    }
    return count;
}
