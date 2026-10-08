/* The C host of probe.ae, built against the --emit-header file (#2517). It
 * sends Mixed twice, through the typed helper and as the header's struct
 * through aether_send_message, then Bump through its helper, which travels
 * inline. The actor prints the fields it reads. */
#include "probe.h"

int host_send(void* actor) {
    Sink_Mixed((Sink*)actor, 7, "seven", 8, 2.5, 9, (void*)0);
    Mixed m = { ._message_id = MSG_Mixed, .a = 70, .s = "seventy", .b = 80,
                .f = 0.25, .c = 90, .p = (void*)0 };
    aether_send_message(actor, &m, sizeof m);
    Sink_Bump((Sink*)actor, 5);
    return (int)sizeof(Mixed);
}
