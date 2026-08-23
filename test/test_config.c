/* Host test for vs_parse_addr. A bad parse strands the app with an
 * unreachable server and no DNS fallback, so the edge cases matter. */
#define VS_CONFIG_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include "../src/config.h"

static int bad;

static void ok_case(const char *in, const char *want_ip, int want_port,
                    int start_port)
{
    char ip[VS_IP_MAX] = "";
    int  port = start_port;
    int  r = vs_parse_addr(in, ip, sizeof(ip), &port);

    if (!r || strcmp(ip, want_ip) || port != want_port) {
        printf("  FAIL %-28s -> r=%d ip=%s port=%d (want %s:%d)\n",
               in, r, ip, port, want_ip, want_port);
        bad++;
    } else {
        printf("  ok   %-28s -> %s:%d\n", in, ip, port);
    }
}

static void rej_case(const char *in)
{
    char ip[VS_IP_MAX] = "";
    int  port = 8480;
    if (vs_parse_addr(in, ip, sizeof(ip), &port)) {
        printf("  FAIL %-28s accepted, should reject (got %s)\n", in, ip);
        bad++;
    } else {
        printf("  ok   %-28s rejected\n", in);
    }
}

int main(void)
{
    printf("vs_parse_addr tests\n");

    ok_case("192.168.1.10",              "192.168.1.10", 8480, 8480);
    ok_case("192.168.1.10:9000",         "192.168.1.10", 9000, 8480);
    ok_case("  10.0.0.5 : 80 ",          "10.0.0.5",       80, 8480);
    ok_case("http://192.168.1.10:8480/", "192.168.1.10", 8480, 1);
    ok_case("https://10.1.2.3",          "10.1.2.3",     8480, 8480);
    ok_case("0.0.0.0",                   "0.0.0.0",      8480, 8480);
    ok_case("255.255.255.255:65535",     "255.255.255.255", 65535, 1);

    rej_case("myserver.local");        /* no DNS in http.h */
    rej_case("192.168.1");             /* too few octets */
    rej_case("192.168.1.10.5");        /* too many */
    rej_case("192.168.1.256");         /* octet overflow */
    rej_case("192.168..10");           /* empty octet */
    rej_case("192.168.1.10:0");        /* invalid port */
    rej_case("192.168.1.10:99999");    /* port overflow */
    rej_case("");
    rej_case("::1");

    printf("\n%s (%d failure%s)\n", bad ? "FAILED" : "PASSED", bad,
           bad == 1 ? "" : "s");
    return bad != 0;
}
