#include <stdio.h>
#include <libmill.h>
#include <errno.h>

coroutine void worker(tcpsock s) {
    ipaddr peer = tcpaddr(s);
    char buf[IPADDR_MAXSTRLEN];
    char *addr = ipaddrstr(peer, buf);
    printf("Incomming: %s!\n", addr);
    fflush(stdout);
    char buffer[256];
    while (1)
    {
        /*
         * tcprecv tries to fill the FULL requested buffer before returning.
         * Use a short deadline (100ms) so it returns quickly with whatever
         * data is available.  ETIMEDOUT just means "no more data right now",
         * which is fine - we echo what we got and go back for more.
         */
        size_t nbytes = tcprecv(s, buffer, sizeof(buffer), now() + 100);
        int recv_err = errno;

        if (nbytes > 0) {
            /* We got data - echo it back */
            tcpsend(s, buffer, nbytes, -1);
            if (errno != 0) goto close;
            tcpflush(s, -1);
            if (errno != 0) goto close;
        }
        else if (recv_err == ECONNRESET || recv_err == 0) {
            /* Connection closed by peer */
            goto close;
        }
        else if (recv_err == ETIMEDOUT) {
            /* No data yet - just loop and try again */
            continue;
        }
        else {
            /* Real error */
            goto close;
        }
    }
close:{
        printf("Disconnected: %s!\n", addr);
        fflush(stdout);
        tcpclose(s);
    }
}

coroutine void client_dispatcher(tcpsock ls) {
    while(1) {
        tcpsock s = tcpaccept(ls, -1);
        go(worker(s));
    }
}

int main() {
    ipaddr addr = iplocal(NULL, 5555, 0);
    tcpsock ls = tcplisten(addr, 10);
    go(client_dispatcher(ls));
    msleep(-1);
    return 0;
}
