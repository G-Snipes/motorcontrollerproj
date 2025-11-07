/* motor_controller_postgres.c
   PostgreSQL based motor controller (thread-safe and crash-free).
   Compile:
     gcc -I/usr/local/opt/libpq/include motor_controller_pgsql.c -o motor_controller_pgsql -L/usr/local/opt/libpq/lib -lpq -lpthread -lm
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <libpq-fe.h> // Correct header
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

void handle_client_socket(int client_fd, PGconn *conn);

#define TELEMETRY_INTERVAL_MS 200
#define COMMAND_POLL_INTERVAL_MS 100
#define COMMAND_IGNORE_MS 200
#define TCP_PORT 9090
#define LISTEN_BACKLOG 5
#define CLIENT_ID_SIZE 128

typedef struct {
    double gas_level;
    double battery_level;
    double motor_speed;
    double motor_speed_set_point;
    double motor_temp;
    pthread_mutex_t lock;
} MotorState;

MotorState state;

/* PosgreSQL connection string construction */
const char *db_conninfo = "host=127.0.0.1 port=5432 dbname=motordb user=motoruser password=MotorPass123!"; // Correct connection string
unsigned int db_port = 5432; // Kept for reference, but included in db_conninfo

long long last_processed_ms = 0;
pthread_mutex_t last_processed_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t db_init_lock = PTHREAD_MUTEX_INITIALIZER;

long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

void msleep(long ms) {
    struct timespec req = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&req, NULL);
}

// *** CHANGE 1: Corrected escape_string to use PQescapeString, with proper error check and null termination ***
// Safe escape helper: writes to stack buffer
void escape_string(PGconn *conn, const char *src, char *dst, size_t dst_size) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    // PQescapeString requires a buffer of size at least (2 * len + 1)
    if (len * 2 + 1 > dst_size) {
        fprintf(stderr, "escape_string: buffer too small (src_len=%zu, dst_size=%zu)\n", len, dst_size);
        dst[0] = '\0';
        return;
    }
    
    // PQescapeString returns the length of the escaped string (excluding the null terminator)
    int out_len = PQescapeString(dst, src, len);
    
    if (out_len < 0) {
        fprintf(stderr, "escape_string: failed to escape string\n");
        dst[0] = '\0';
        return;
    }
    // No explicit null terminator needed as PQescapeString handles it, but good practice for safety
    dst[out_len] = '\0';
}


/* Create database and tables if missing */
int init_db(PGconn *conn) {

    pthread_mutex_lock(&db_init_lock); //lock to prevent race conditions

    const char *telemetry_table =
        "CREATE TABLE IF NOT EXISTS telemetry ("
        "id SERIAL PRIMARY KEY,"
        "ts TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,"
        "gas_level DOUBLE PRECISION,"
        "battery_level DOUBLE PRECISION,"
        "motor_speed DOUBLE PRECISION,"
        "motor_speed_set_point DOUBLE PRECISION,"
        "motor_temp DOUBLE PRECISION)";
        
    PGresult *res = PQexec(conn, telemetry_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to create telemetry table: %s\n", PQerrorMessage(conn));
        PQclear(res); return 0;
    }
    PQclear(res);

    const char *commands_table =
        "CREATE TABLE IF NOT EXISTS commands ("
        "id SERIAL PRIMARY KEY,"
        "client_id VARCHAR(128),"
        "percent_change DOUBLE PRECISION,"
        "issued_via VARCHAR(32),"
        "processed SMALLINT DEFAULT 0,"
        "ts TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,"
        "processed_ts TIMESTAMP WITH TIME ZONE NULL,"
        "processed_by VARCHAR(64) NULL)";

    res = PQexec(conn, commands_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to create commands table: %s\n", PQerrorMessage(conn));
        PQclear(res); return 0;
    }
    PQclear(res);
    return 1;

    pthread_mutex_unlock(&db_init_lock); //release lock
}

/* Insert telemetry row */
void insert_telemetry(PGconn *conn, MotorState *s) {
    char q[512];
    char b1[64], b2[64], b3[64], b4[64], b5[64];
    pthread_mutex_lock(&s->lock);
    snprintf(b1,sizeof(b1),"%.3f", s->gas_level);
    snprintf(b2,sizeof(b2),"%.3f", s->battery_level);
    snprintf(b3,sizeof(b3),"%.3f", s->motor_speed);
    snprintf(b4,sizeof(b4),"%.3f", s->motor_speed_set_point);
    snprintf(b5,sizeof(b5),"%.3f", s->motor_temp);
    pthread_mutex_unlock(&s->lock);

    snprintf(q, sizeof(q),
        "INSERT INTO telemetry (gas_level, battery_level, motor_speed, motor_speed_set_point, motor_temp) "
        "VALUES (%s, %s, %s, %s, %s)",
        b1, b2, b3, b4, b5);

    PGresult *res = PQexec(conn, q);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Telemetry insert failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);
}

/* Insert command (from TCP clients) */
void insert_command(PGconn *conn, const char *client_id, double percent, const char *issued_via) {
    char q[512];
    char percent_s[64];
    snprintf(percent_s, sizeof(percent_s), "%.6f", percent);

    char esc_client[256], esc_via[256];
    escape_string(conn, client_id ? client_id : "tcp_client", esc_client, sizeof(esc_client));
    escape_string(conn, issued_via ? issued_via : "tcp", esc_via, sizeof(esc_via));

    snprintf(q, sizeof(q),
        "INSERT INTO commands (client_id, percent_change, issued_via) VALUES ('%s', %s, '%s')",
        esc_client, percent_s, esc_via);

    // *** CHANGE 2 & 3: Replaced pgsql_query and pgsql_error with PQexec and proper status check ***
    PGresult *res = PQexec(conn, q);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Command insert failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);
}

// Poll and process commands 
void poll_and_process_commands(PGconn *conn, MotorState *s) {
    const char *select_q = "SELECT id, client_id, percent_change, ts FROM commands WHERE processed = 0 ORDER BY ts ASC LIMIT 1";    

    // *** CHANGE 4: Removed old MySQL-style query/error check ***
    // if (pgsql_query(conn, q)) { fprintf(stderr, "Command select failed: %s\n", pgsql_error(conn)); return; }

    PGresult *res = PQexec(conn, select_q);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) { // Check for successful SELECT
        fprintf(stderr, "Command select failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        return;
    }

    int rows = PQntuples(res);
    if (rows == 0) { // No commands found
        PQclear(res);
        return;
    }

    // *** CHANGE 5: Replaced MySQL-style loop/fetch with direct PGresult access for LIMIT 1 ***
    // Note: Since we use LIMIT 1, we access row 0 directly.
    char *id = PQgetvalue(res, 0, 0);          
    char *client_id = PQgetvalue(res, 0, 1);
    char *percent = PQgetvalue(res, 0, 2);

    long long now = now_ms();
    pthread_mutex_lock(&last_processed_lock);
    long long delta = now - last_processed_ms;
    if (delta < COMMAND_IGNORE_MS) {
        pthread_mutex_unlock(&last_processed_lock);
        PQclear(res); // Clear the select result before returning
        return; // Skip command
    }
    pthread_mutex_unlock(&last_processed_lock);

    double p = atof(percent);
    pthread_mutex_lock(&s->lock);
    s->motor_speed_set_point += s->motor_speed_set_point * (p / 100.0);
    if (s->motor_speed_set_point < 0) s->motor_speed_set_point = 0;
    if (s->motor_speed_set_point > 10000) s->motor_speed_set_point = 10000;
    pthread_mutex_unlock(&s->lock);

    char uq[256];
    // Note: PostgreSQL uses 'CURRENT_TIMESTAMP' for current time
    snprintf(uq, sizeof(uq),
         "UPDATE commands SET processed=1, processed_ts = CURRENT_TIMESTAMP, processed_by = 'motor_controller' WHERE id = %s",
         id);
         
    // Execute the Update query
    PGresult *update_res = PQexec(conn, uq); // Use a new result variable for the update
    if (PQresultStatus(update_res) != PGRES_COMMAND_OK) {
     fprintf(stderr, "Failed to mark processed: %s\n", PQerrorMessage(conn));
    }
    PQclear(update_res); // Clear the update result

    pthread_mutex_lock(&last_processed_lock);
    last_processed_ms = now;
    pthread_mutex_unlock(&last_processed_lock);

    printf("[controller] applied command id=%s from=%s percent=%s at ms=%lld\n", id, client_id, percent, now);
    
    PQclear(res); // *** CHANGE 6: Clear the SELECT result (was mistakenly cleared earlier in this logic block) ***
}

//PID struct
typedef struct {
// ... (PID struct and pid_step function are fine) ...
    double kp, ki, kd;
    double prev_err;
    double integral;
} PID;

double pid_step(PID *pid, double setpoint, double measure, double dt) {
    double err = setpoint - measure;
    pid->integral += err * dt;
    double derivative = dt > 0 ? (err - pid->prev_err) / dt : 0;
    pid->prev_err = err;
    return pid->kp * err + pid->ki * pid->integral + pid->kd * derivative;
}

// New PostgreSQL Connection Function
PGconn *thread_db_connect() {
    // *** CHANGE 7: Use the db_conninfo string for connection ***
    PGconn *conn = PQconnectdb(db_conninfo);
    
    // Check connection status
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "Thread DB connection failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }
    return conn;
}

// Telemetry thread 
void *telemetry_thread(void *arg) {
    PGconn *conn = thread_db_connect();
    if (!conn) return NULL;
    if (!init_db(conn)) return NULL;

    PID pid = { .kp = 0.5, .ki = 0.1, .kd = 0.05, .prev_err = 0, .integral = 0 };
    double dt = 0.2;

    while (1) {
        pthread_mutex_lock(&state.lock);
        state.gas_level -= 0.02; if (state.gas_level < 0) state.gas_level = 0;
        state.battery_level -= 0.01; if (state.battery_level < 0) state.battery_level = 0;
        double control = pid_step(&pid, state.motor_speed_set_point, state.motor_speed, dt);
        double noise = ((rand() % 100) - 50) / 100.0;
        state.motor_speed += (control * 0.1 + noise) * dt;
        if (state.motor_speed < 0) state.motor_speed = 0;
        state.motor_temp = 20.0 + state.motor_speed * 0.01 + ((rand()%100)/100.0 - 0.5);
        pthread_mutex_unlock(&state.lock);

        insert_telemetry(conn, &state);
        msleep(TELEMETRY_INTERVAL_MS);
    }

    // *** CHANGE 8: Replaced mysql_close/pgsql_close with PQfinish ***
    PQfinish(conn);
    return NULL;
}

// Command poller thread 
void *command_poller_thread(void *arg) {
    printf("[Poller] Attempting DB connection...\n");
    PGconn *conn = thread_db_connect();
    if (!conn) {
        fprintf(stderr, "[Poller] Failed to connect to DB. Exiting thread.\n");
        return NULL;
    }
    printf("[Poller] DB connected.\n");
    
    if (!init_db(conn)) {
        fprintf(stderr, "[Poller] Failed to initialize DB/Select DB. Exiting thread.\n");
        return NULL;
    }
    printf("[Poller] DB initialized. Starting poll loop.\n");

    while (1) {
        poll_and_process_commands(conn, &state);
        msleep(COMMAND_POLL_INTERVAL_MS);
    }   

    // *** CHANGE 9: Replaced mysql_close/pgsql_close with PQfinish ***
    PQfinish(conn);
    return NULL;
}

// TCP client handler (no changes needed here)
// TCP client handler 
void handle_client_socket(int client_fd, PGconn *conn) {
    char buf[256];
    ssize_t r = read(client_fd, buf, sizeof(buf)-1);  //makes sure date from client isn't empty
    if (r <= 0) { close(client_fd); return; }
    buf[r] = 0;

    char client_id[128] = {0};
    double percent = 0.0;
    if (sscanf(buf, "%127s %lf", client_id, &percent) >= 1) {
        if (strlen(client_id) == 0) strncpy(client_id, "tcp_client", sizeof(client_id)-1);
        insert_command(conn, client_id, percent, "tcp");
        const char *successmessage = "Parse successful\n";
        write(client_fd, successmessage, strlen(successmessage));
    } else {
        const char *failedmessage = "Parse failed.\n";
        write(client_fd, failedmessage, strlen(failedmessage));
    }
    close(client_fd);
}

// TCP server thread 
void *tcp_server_thread(void *arg) {
    PGconn *conn = thread_db_connect();
    if (!conn) return NULL;
    if (!init_db(conn)) return NULL;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return NULL; }
    printf("[tcp] socket created\n");
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TCP_PORT);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(srv); return NULL; }
    printf("[tcp] bind succeeded\n");

    if (listen(srv, LISTEN_BACKLOG) < 0) { perror("listen"); close(srv); return NULL; }
    printf("[tcp] listening on port %d\n", TCP_PORT);

    while (1) {
        int client = accept(srv, NULL, NULL);
        if (client < 0) { perror("accept"); continue; }
        handle_client_socket(client, conn);
    }

    close(srv);
    // *** CHANGE 10: Replaced mysql_close/pgsql_close with PQfinish ***
    PQfinish(conn);
    return NULL;
}

/* Main */
int main() {
    // ... (main is fine) ...
    srand(time(NULL));
    pthread_mutex_init(&state.lock, NULL);
    state.gas_level = 100.0;
    state.battery_level = 100.0;
    state.motor_speed = 0.0;
    state.motor_speed_set_point = 100.0;
    state.motor_temp = 40.0;

    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, telemetry_thread, NULL);
    pthread_create(&t2, NULL, command_poller_thread, NULL);
    pthread_create(&t3, NULL, tcp_server_thread, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    return 0;
}