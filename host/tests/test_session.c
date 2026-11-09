#include "check.h"
#include <stdio.h>
#include <string.h>

#include "session.h"

static void test_parse_hello(void)
{
    const char *json =
        "{\"proto\":33,\"fw\":\"2.1.0\",\"bitrate\":\"500k\",\"mode\":\"LISTEN\","
        "\"device_id\":\"AABBCCDDEEFF\",\"boot\":42,\"uptime_ms\":123456}";
    session_hello_t h;
    CHECK(session_parse_hello((const uint8_t *)json, (uint16_t)strlen(json), &h));
    CHECK(h.proto == 33);
    CHECK(strcmp(h.fw, "2.1.0") == 0);
    CHECK(h.bitrate == 500000);
    CHECK(h.has_device_id && strcmp(h.device_id, "AABBCCDDEEFF") == 0);
    CHECK(h.has_boot && h.boot_count == 42);
    CHECK(h.has_uptime && h.uptime_ms == 123456);

    /* Old firmware: no additive fields. */
    const char *old = "{\"proto\":32,\"fw\":\"2.0.0\",\"bitrate\":\"250k\"}";
    CHECK(session_parse_hello((const uint8_t *)old, (uint16_t)strlen(old), &h));
    CHECK(h.bitrate == 250000);
    CHECK(!h.has_device_id && !h.has_boot && !h.has_uptime);

    CHECK(!session_parse_hello(NULL, 0, &h));
}

static void test_anchor(void)
{
    CHECK(session_anchor_start_wall_ms(1700000000123ull, 123u) == 1700000000000ull);
    CHECK(session_anchor_start_wall_ms(100u, 200u) == 0);
}

static linsight_can_record_t rec(uint16_t seq, uint32_t ts)
{
    linsight_can_record_t r;
    memset(&r, 0, sizeof(r));
    r.seq = seq;
    r.ts_ms = ts;
    return r;
}

static void test_dedupe(void)
{
    session_state_t s;
    memset(&s, 0, sizeof(s));
    session_reset(&s, 1, true);

    linsight_can_record_t a = rec(10, 1000);
    CHECK(session_record_new(&s, &a));
    CHECK(session_record_new(&s, &a) == false); /* exact duplicate */
    CHECK(s.skipped == 1);

    linsight_can_record_t b = rec(11, 1000); /* same ms, next seq */
    CHECK(session_record_new(&s, &b));

    linsight_can_record_t c = rec(14, 1001); /* gap of 2 */
    CHECK(session_record_new(&s, &c));
    CHECK(s.gaps == 1 && s.missing == 2);

    linsight_can_record_t old = rec(9, 999); /* reconnect backlog */
    CHECK(session_record_new(&s, &old) == false);
    CHECK(s.skipped == 2);

    /* seq wrap: 0xFFFF -> 0 is +1. */
    linsight_can_record_t w1 = rec(0xFFFF, 2000);
    linsight_can_record_t w2 = rec(0x0000, 2001);
    CHECK(session_record_new(&s, &w1));
    CHECK(session_record_new(&s, &w2));
    CHECK(s.gaps == 1 && s.missing == 2); /* unchanged */

    /* A device reboot resets the marks; the same old timestamps are valid. */
    session_reset(&s, 2, true);
    linsight_can_record_t fresh = rec(0, 10);
    CHECK(session_record_new(&s, &fresh));
}

static void test_counters(void)
{
    session_state_t s;
    memset(&s, 0, sizeof(s));
    session_reset(&s, 1, true);
    linsight_can_record_t a = rec(1, 1);
    CHECK(session_record_new(&s, &a));
    CHECK(s.accepted == 1);
    session_clear_counters(&s);
    CHECK(s.accepted == 0 && s.skipped == 0 && s.gaps == 0);

    /* Marks survive a counter reset, so a reconnect still skips its backlog. */
    CHECK(session_record_new(&s, &a) == false);
}

int main(void)
{
    test_parse_hello();
    test_anchor();
    test_dedupe();
    test_counters();
    puts("session: ok");
    return 0;
}
