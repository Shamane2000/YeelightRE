/*
 * mcufw_mqtt.c -- MQTT HA Auto-Discovery payload builder for mcufw phase 3.5.
 *
 * Generates the JSON payloads and topic strings for Home Assistant
 * auto-discovery. The actual MQTT client connection is deferred to phase 5
 * (needs real WLAN association + DHCP). This module is pure serialization
 * with no network dependencies, making it fully host-testable.
 */
#include "mcufw_mqtt.h"

/* Append string to buffer at *pos, respecting cap. Advances *pos. */
static void append(char *out, size_t cap, size_t *pos, const char *s)
{
    while (*s && *pos < cap - 1)
        out[(*pos)++] = *s++;
}

/* Append decimal number. */
static void append_dec(char *out, size_t cap, size_t *pos, uint32_t v)
{
    char buf[11];
    int i = 0;
    if (v == 0) { buf[i++] = '0'; }
    else { while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; } }
    while (i > 0 && *pos < cap - 1)
        out[(*pos)++] = buf[--i];
}

/* Append hex (lowercase, no prefix). */
static void append_hex(char *out, size_t cap, size_t *pos, uint32_t v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    for (int d = digits - 1; d >= 0 && *pos < cap - 1; d--)
        out[(*pos)++] = hx[(v >> (d * 4)) & 0xf];
}

int mcufw_mqtt_discovery_topic(uint32_t muid, char *out, size_t cap)
{
    /* homeassistant/light/<muid_hex>/config */
    size_t pos = 0;
    append(out, cap, &pos, "homeassistant/light/");
    append_hex(out, cap, &pos, muid, 8);
    append(out, cap, &pos, "/config");
    out[pos] = '\0';
    return (int)pos;
}

int mcufw_mqtt_state_topic(uint32_t muid, char *out, size_t cap)
{
    size_t pos = 0;
    append(out, cap, &pos, "yeelight/");
    append_hex(out, cap, &pos, muid, 8);
    append(out, cap, &pos, "/state");
    out[pos] = '\0';
    return (int)pos;
}

int mcufw_mqtt_cmd_topic(uint32_t muid, char *out, size_t cap)
{
    size_t pos = 0;
    append(out, cap, &pos, "yeelight/");
    append_hex(out, cap, &pos, muid, 8);
    append(out, cap, &pos, "/set");
    out[pos] = '\0';
    return (int)pos;
}

int mcufw_mqtt_discovery_light(uint32_t muid, char *out, size_t cap)
{
    /* Minimal HA MQTT discovery payload for a light with brightness + CCT + RGB. */
    size_t pos = 0;
    char muid_hex[9];
    {
        size_t hp = 0;
        append_hex(muid_hex, sizeof(muid_hex), &hp, muid, 8);
        muid_hex[hp] = '\0';
    }

    append(out, cap, &pos, "{\"name\":\"Yeelight ");
    append(out, cap, &pos, muid_hex);
    append(out, cap, &pos, "\",\"unique_id\":\"yeelight_");
    append(out, cap, &pos, muid_hex);
    append(out, cap, &pos, "\",\"schema\":\"json\",\"brightness\":true,"
           "\"color_temp\":true,\"rgb\":true,"
           "\"command_topic\":\"yeelight/");
    append(out, cap, &pos, muid_hex);
    append(out, cap, &pos, "/set\",\"state_topic\":\"yeelight/");
    append(out, cap, &pos, muid_hex);
    append(out, cap, &pos, "/state\"}");
    out[pos] = '\0';
    return (int)pos;
}

int mcufw_mqtt_state_payload(int power, uint8_t bright, uint16_t ct,
                             uint16_t hue, uint8_t sat,
                             char *out, size_t cap)
{
    size_t pos = 0;
    append(out, cap, &pos, "{\"power\":\"");
    append(out, cap, &pos, power ? "on" : "off");
    append(out, cap, &pos, "\",\"bright\":");
    append_dec(out, cap, &pos, bright);
    append(out, cap, &pos, ",\"ct\":");
    append_dec(out, cap, &pos, ct);
    append(out, cap, &pos, ",\"hue\":");
    append_dec(out, cap, &pos, hue);
    append(out, cap, &pos, ",\"sat\":");
    append_dec(out, cap, &pos, sat);
    append(out, cap, &pos, "}");
    out[pos] = '\0';
    return (int)pos;
}
