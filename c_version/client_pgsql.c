/* client_pgsql.c
   PostgreSQL-based client: optionally send TCP command, and poll DB every 250 ms.
   Compile:
     gcc -I/usr/local/opt/libpq/include client_pgsql.c -o client_pgsql -L/usr/local/opt/libpq/lib -lpq -lm
   Usage:
     ./client_pgsql <client_id> [send_percent]
*/

#define _POSIX_C_SOURCE 200809L 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h> // Not strictly needed for this single-threaded client, but kept.
#include <unistd.h>
#include <math.h>
#include <libpq-fe.h> 
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <arpa/inet.h>

// --- CHANGE 2: Use the standard PostgreSQL connection string ---
const char *db_conninfo = "host=127.0.0.1 port=5432 dbname=motordb user=motoruser password=MotorPass123!";
// MySQL port 3306 is no longer relevant, but the TCP port 9090 is correct.
#define TCP_PORT 9090
#define TCP_HOST "127.0.0.1"
#define POLL_MS 250

long long now_ms() {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec*1000LL + ts.tv_nsec/1000000LL;
}

void msleep(long ms) {
    struct timespec req = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&req, NULL);
}

// TCP send function remains the same as it connects to the same TCP server.
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

    // --- CHANGE 3: Initialize and connect using PQconnectdb ---
    PGconn *conn = PQconnectdb(db_conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "DB connection failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn); return 1;
    }
    
    // Check for the table's existence (optional but robust)
    PGresult *check_res = PQexec(conn, "SELECT 1 FROM commands LIMIT 1");
    if (PQresultStatus(check_res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Error: Commands table not found or accessible. Is motor_controller running?\n");
        PQclear(check_res);
        PQfinish(conn);
        return 1;
    }
    PQclear(check_res);


    if (will_send) {
        printf("[client] sending %+.3f via TCP to controller\n", send_percent);
        send_tcp_command(client_id, send_percent);
    }

    long long last_id = 0;
    while (1) {
        char q[256];
        // Note: PostgreSQL uses BIGINT for long long comparison
        snprintf(q, sizeof(q), "SELECT id, client_id, percent_change, ts FROM commands WHERE id > %lld ORDER BY id ASC", last_id);
        
        // --- CHANGE 4: Execute query using PQexec ---
        PGresult *res = PQexec(conn, q);
        
        // --- CHANGE 5: Check result status and handle error ---
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            fprintf(stderr, "select failed: %s\n", PQerrorMessage(conn));
            PQclear(res);
            msleep(POLL_MS);
            continue;
        }

        // --- CHANGE 6: Iterate through PostgreSQL results ---
        int rows = PQntuples(res);
        if (rows > 0) {
            for (int i = 0; i < rows; i++) {
                // PQgetvalue access: (result_set, row_index, column_index)
                long long id = atoll(PQgetvalue(res, i, 0));
                const char *cid = PQgetvalue(res, i, 1);
                const char *percent = PQgetvalue(res, i, 2);
                const char *ts = PQgetvalue(res, i, 3);
                
                printf("[cmd] id=%lld from=%s percent=%s ts=%s\n", id, cid, percent, ts);
                if (id > last_id) last_id = id;
            }
        }
        
        // --- CHANGE 7: Always clear the result object ---
        PQclear(res);

        usleep(POLL_MS * 1000); //sleeps for 250ms
    }

    // --- CHANGE 8: Close connection using PQfinish ---
    PQfinish(conn);
    return 0;
}