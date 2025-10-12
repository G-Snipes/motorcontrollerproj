/* client_mysql.c
   MySQL-based client: optionally send TCP command, and poll DB every 250 ms.
   Compile:
     gcc client_mysql.c -o client_mysql -lmysqlclient -lm
   Usage:
     ./client_mysql <client_id> [send_percent]
*/

#define _POSIX_C_SOURCE 200809L 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <mysql/mysql.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <arpa/inet.h>

const char *db_host = "127.0.0.1";
const char *db_user = "motoruser";
const char *db_pass = "MotorPass123!";
const char *db_name = "motordb";
unsigned int db_port = 3306;

#define TCP_PORT 9090
#define TCP_HOST "127.0.0.1"
#define POLL_MS 250 //clients read from the database every 250ms as required by spec

long long now_ms() {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec*1000LL + ts.tv_nsec/1000000LL;
}

void msleep(long ms) {
    struct timespec req = { ms / 1000, (ms % 1000) * 1000000L }; //gets the exact amount of seconds to sleep
    nanosleep(&req, NULL); //more precise sleep function (points to the req timespec for handling)
}

void send_tcp_command(const char *client_id, double percent) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }
    struct sockaddr_in srv;
    srv.sin_family = AF_INET;
    srv.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, TCP_HOST, &srv.sin_addr);
    if (connect(sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect");
        close(sock);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %.6f", client_id, percent);
    write(sock, buf, strlen(buf));
    char rbuf[64];
    ssize_t r = read(sock, rbuf, sizeof(rbuf)-1);
    if (r>0) { rbuf[r]=0; printf("[tcp] reply: %s", rbuf); }
    close(sock);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <client_id> [send_percent]\n", argv[0]); return 1; }
    const char *client_id = argv[1];
    double send_percent = 0;
    int will_send = 0;
    if (argc >= 3) { will_send = 1; send_percent = atof(argv[2]); }

    MYSQL *conn = mysql_init(NULL);
    if (!conn) { fprintf(stderr, "mysql_init failed\n"); return 1; }
    if (!mysql_real_connect(conn, db_host, db_user, db_pass, db_name, db_port, NULL, 0)) {
        fprintf(stderr, "DB connection failed: %s\n", mysql_error(conn));
        mysql_close(conn); return 1;
    }

    if (will_send) {
        printf("[client] sending %+.3f via TCP to controller\n", send_percent);
        send_tcp_command(client_id, send_percent);
    }

    long long last_id = 0;
    while (1) {
        char q[256];
        snprintf(q, sizeof(q), "SELECT id, client_id, percent_change, ts FROM commands WHERE id > %lld ORDER BY id ASC", last_id);
        if (mysql_query(conn, q)) {
            fprintf(stderr, "select failed: %s\n", mysql_error(conn));
            msleep(POLL_MS);
            continue;
        }
        MYSQL_RES *res = mysql_store_result(conn);
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                long long id = atoll(row[0]);
                const char *cid = row[1];
                const char *percent = row[2];
                const char *ts = row[3];
                printf("[cmd] id=%lld from=%s percent=%s ts=%s\n", id, cid, percent, ts);
                if (id > last_id) last_id = id;
            }
            mysql_free_result(res);
        }
        usleep(POLL_MS * 1000); //sleeps for 250ms
    }

    mysql_close(conn);
    return 0;
}
