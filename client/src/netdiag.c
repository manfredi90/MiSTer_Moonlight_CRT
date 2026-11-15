/* The MiSTer's network diagnosis, written to the log when something goes wrong:
 * Wi-Fi state, the PC's ARP entry, ping and the kernel's last messages. */
#include "netdiag.h"

#include <stdio.h>
#include <string.h>

void mg_net_diag(const char *why, const char *host) {
  char cmd[512];
  /* host is an IP address or a name from the .conf: no shell characters */
  for (const char *c = host; *c; c++)
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-'))
      return;
  snprintf(cmd, sizeof(cmd),
           "date +%%T; echo uptime $(cut -d' ' -f1 /proc/uptime) s; ip route 2>&1 | head -3; ip -s neigh show %s 2>&1; "
           "for w in /sys/class/net/wlan*; do [ -e $w ] && iw dev $(basename $w) link 2>&1 | head -6; done; "
           "ping -c 2 -W 1 %s 2>&1 | tail -2; "
           "dmesg 2>/dev/null | grep -v -i -E 'MiSTer_fb|fpga' | tail -25",
           host, host);
  printf("[net] %s: diagnosis\n", why);
  fflush(stdout);
  FILE *p = popen(cmd, "r");
  if (!p)
    return;
  char line[256];
  while (fgets(line, sizeof(line), p))
    printf("[net]   %s", line);
  pclose(p);
  fflush(stdout);
}
