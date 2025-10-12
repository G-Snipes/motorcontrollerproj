/* motor_controller_mysql_safe.c
   MySQL-based motor controller (thread-safe and crash-free).
   Compile:
     gcc motor_controller_mysql_safe.c -o motor_controller_mysql_safe \
         -lmysqlclient -lpthread -lm -I/usr/local/opt/mysql/include -L/usr/local/opt/mysql/lib
*/

#define _POSIX_C_SOURCE 200809L //
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

#define TELEMETRY_INTERVAL_MS 200
#define COMMAND_POLL_INTERVAL_MS 100
#define COMMAND_IGNORE_MS 200
#define TCP_PORT 9090
#define LISTEN_BACKLOG 5

typedef struct {
    double gas_level;
    double battery_level;
    double motor_speed;
    double motor_speed_set_point;
    double motor_temp;
    pthread_mutex_t lock;
} MotorState;

MotorState state;

/* MySQL struct connection parameters */
const char *db_host = "127.0.0.1";
const char *db_user = "motoruser";
const char *db_pass = "MotorPass123!";
const char *db_name = "motordb";
unsigned int db_port = 3306;

long long last_processed_ms = 0;
pthread_mutex_t last_processed_lock = PTHREAD_MUTEX_INITIALIZER;

long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

void msleep(long ms) {
    struct timespec req = { ms / 1000, (ms % 1000) * 1000000L }; //gets the exact amount of seconds to sleep
    nanosleep(&req, NULL); //more precise sleep function (points to the req timespec for handling)
}

// Safe escape helper: writes to stack buffer
void escape_string(MYSQL *conn, const char *src, char *dst, size_t dst_size) { //prevents against SQL Injection
    unsigned long len = strlen(src);
    if (len * 2 + 1 > dst_size) {
        fprintf(stderr, "escape_string: buffer too small\n");
        dst[0] = '\0';
        return;
    }
    unsigned long out_len = mysql_real_escape_string(conn, dst, src, len);
    dst[out_len] = '\0';
}

/* Create database and tables if missing */
int init_db(MYSQL *conn) {
    if (mysql_query(conn, "CREATE DATABASE IF NOT EXISTS motordb")) {
        fprintf(stderr, "Failed to create database: %s\n", mysql_error(conn));
        return 0;
    }
    if (mysql_select_db(conn, db_name)) {
        fprintf(stderr, "Failed to select database: %s\n", mysql_error(conn));
        return 0;
    }

    const char *telemetry_table =
        "CREATE TABLE IF NOT EXISTS telemetry ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "gas_level DOUBLE,"
        "battery_level DOUBLE,"
        "motor_speed DOUBLE,"
        "motor_speed_set_point DOUBLE,"
        "motor_temp DOUBLE)";
    if (mysql_query(conn, telemetry_table)) {
        fprintf(stderr, "Failed to create telemetry table: %s\n", mysql_error(conn));
        return 0;
    }

    const char *commands_table =
        "CREATE TABLE IF NOT EXISTS commands ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "client_id VARCHAR(128),"
        "percent_change DOUBLE,"
        "issued_via VARCHAR(32),"
        "processed TINYINT DEFAULT 0,"
        "ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "processed_ts TIMESTAMP NULL,"
        "processed_by VARCHAR(64) NULL)";
    if (mysql_query(conn, commands_table)) {
        fprintf(stderr, "Failed to create commands table: %s\n", mysql_error(conn));
        return 0;
    }
    return 1;
}

/* Insert telemetry row */
void insert_telemetry(MYSQL *conn, MotorState *s) {
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

    if (mysql_query(conn, q)) {
        fprintf(stderr, "Telemetry insert failed: %s\n", mysql_error(conn));
    }
}

/* Insert command (from TCP clients) */
void insert_command(MYSQL *conn, const char *client_id, double percent, const char *issued_via) {
    char q[512];
    char percent_s[64];
    snprintf(percent_s, sizeof(percent_s), "%.6f", percent);

    char esc_client[256], esc_via[256];
    escape_string(conn, client_id ? client_id : "tcp_client", esc_client, sizeof(esc_client)); //if client id is not null allow it to be, otherwise mark it as tcp
    escape_string(conn, issued_via ? issued_via : "tcp", esc_via, sizeof(esc_via));

    snprintf(q, sizeof(q),
        "INSERT INTO commands (client_id, percent_change, issued_via) VALUES ('%s', %s, '%s')",
        esc_client, percent_s, esc_via);

    if (mysql_query(conn, q)) {
        fprintf(stderr, "Command insert failed: %s\n", mysql_error(conn));
    }
}

// Poll and process commands 
void poll_and_process_commands(MYSQL *conn, MotorState *s) {
    const char *q = "SELECT id, client_id, percent_change, ts FROM commands WHERE processed = 0 ORDER BY ts ASC"; 
    if (mysql_query(conn, q)) { //queries for the most recent command
        fprintf(stderr, "Command select failed: %s\n", mysql_error(conn));
        return;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) { //go through each command we polled row by row. 
        char *id = row[0];
        char *client_id = row[1];
        char *percent = row[2];

        long long now = now_ms(); //gets current time
        pthread_mutex_lock(&last_processed_lock);
        long long delta = now - last_processed_ms; //starts at 0 as declared in header for first process
        if (delta < COMMAND_IGNORE_MS) { //makes sure it hasn't been 200ms, skips command
            pthread_mutex_unlock(&last_processed_lock);
            continue;
        }
        pthread_mutex_unlock(&last_processed_lock); 

        double p = atof(percent); //converts percent string into a floating point number
        pthread_mutex_lock(&s->lock); //locks resources from other threads for processing 
        s->motor_speed_set_point += s->motor_speed_set_point * (p / 100.0); //gets new percentage
        if (s->motor_speed_set_point < 0) s->motor_speed_set_point = 0;  //min clamp
        if (s->motor_speed_set_point > 10000) s->motor_speed_set_point = 10000;  //max clamp
        pthread_mutex_unlock(&s->lock);

        char uq[256];
        snprintf(uq, sizeof(uq),
                 "UPDATE commands SET processed=1, processed_ts = CURRENT_TIMESTAMP, processed_by = 'motor_controller' WHERE id = %s",
                 id);
        if (mysql_query(conn, uq)) { //marks command as processed in database yay!
            fprintf(stderr, "Failed to mark processed: %s\n", mysql_error(conn));
        }

        pthread_mutex_lock(&last_processed_lock);
        last_processed_ms = now; //updates timestamp of when most recent process was completed
        pthread_mutex_unlock(&last_processed_lock);

        printf("[controller] applied command id=%s from=%s percent=%s at ms=%lld\n", id, client_id, percent, now);
    }
    mysql_free_result(res); //frees result to prevent memory leakage.
}

//PID struct
typedef struct {
    double kp, ki, kd;
    double prev_err;
    double integral;
} PID;

double pid_step(PID *pid, double setpoint, double measure, double dt) {
    double err = setpoint - measure;                                        //error is the desired setpoint minus the current measurement
    pid->integral += err * dt;                                              //accumulates the past errors over time
    double derivative = dt > 0 ? (err - pid->prev_err) / dt : 0;            //gets derivative, if dt is 0 then makes it 0
    pid->prev_err = err;                                                    //previous error now equals the current error
    return pid->kp * err + pid->ki * pid->integral + pid->kd * derivative;  //returns the summation of the P, I, and D. This value will be applied to the current motor speed. Can be positive or negative. 
}

// Connects threads to MYSQL server
MYSQL *thread_db_connect() {
    MYSQL *conn = mysql_init(NULL);
    if (!conn) return NULL;
    if (!mysql_real_connect(conn, db_host, db_user, db_pass, db_name, db_port, NULL, 0)) { //values already initalized
        fprintf(stderr, "Thread DB connection failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }
    return conn;
}

// Telemetry thread 
void *telemetry_thread(void *arg) {
    MYSQL *conn = thread_db_connect();  //makes a new connection to the server for this thread
    if (!conn) return NULL;             //makes sure connection was successful
    if (!init_db(conn)) return NULL;    //initializes database if it hasn't already

    PID pid = { .kp = 0.5, .ki = 0.1, .kd = 0.05, .prev_err = 0, .integral = 0 }; //initalizes PID values
    double dt = 0.2; //dt interval needs to be in seconds not ms

    while (1) {
        pthread_mutex_lock(&state.lock);
        state.gas_level -= 0.02; if (state.gas_level < 0) state.gas_level = 0;               //we lose a little gas each 
        state.battery_level -= 0.01; if (state.battery_level < 0) state.battery_level = 0;   //we lose a little battery
        double control = pid_step(&pid, state.motor_speed_set_point, state.motor_speed, dt); //run the loop
        double noise = ((rand() % 100) - 50) / 100.0;                                        //*random error!
        state.motor_speed += (control * 0.1 + noise) * dt;                                   //new motor speed is (the new calculated control speed * 0.1 "to provide a realistic scale factor" + our simulated noise ) * elapsed time which is 0.2s or 200ms
        if (state.motor_speed < 0) state.motor_speed = 0;                                    //speed can't be less than 0
        state.motor_temp = 20.0 + state.motor_speed * 0.01 + ((rand()%100)/100.0 - 0.5);     //temp caclulated motor speed plus some random error
        pthread_mutex_unlock(&state.lock);                                                   

        insert_telemetry(conn, &state);  //insert new values into database
        msleep(TELEMETRY_INTERVAL_MS);   //sleep 200ms as resquested by design spec
    }

    mysql_close(conn);
    return NULL;
}

// Command poller thread 
void *command_poller_thread(void *arg) {
    MYSQL *conn = thread_db_connect();    //connect to the server
    if (!conn) return NULL;
    if (!init_db(conn)) return NULL;

    while (1) {
        poll_and_process_commands(conn, &state);
        msleep(COMMAND_POLL_INTERVAL_MS);    //controller reads from the database every 100ms as required by the spec
    }

    mysql_close(conn);
    return NULL;
}

// TCP client handler 
void handle_client_socket(int client_fd, MYSQL *conn) {
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
    MYSQL *conn = thread_db_connect();
    if (!conn) return NULL;
    if (!init_db(conn)) return NULL;

    int srv = socket(AF_INET, SOCK_STREAM, 0);                      //creates the TCP socket, says we are using address family: Internet meaning IPv4, Socket Stream is type tcp
    if (srv < 0) { perror("socket"); return NULL; }
    printf("[tcp] socket created\n");  
    int opt = 1; //4 bytes
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));   //server will bind to the same port

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;          //IPv4
    addr.sin_addr.s_addr = INADDR_ANY;  //"Listen to all network interfaces 0.0.0.0"
    addr.sin_port = htons(TCP_PORT);    //host to network short -> network byte order is Big-endian

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(srv); return NULL; } //attaches socket to port
    printf("[tcp] bind succeeded\n"); 

    if (listen(srv, LISTEN_BACKLOG) < 0) { perror("listen"); close(srv); return NULL; } //socket is put into passive mode so it can listen for new connections
    printf("[tcp] listening on port %d\n", TCP_PORT);

    while (1) {                                             //keeps adding client commands to the commands table
        int client = accept(srv, NULL, NULL);               //waits for client to connect
        if (client < 0) { perror("accept"); continue; }     //if file descriptor is less than 0 than there was an error
        handle_client_socket(client, conn);                 //processes the client
    }

    close(srv);
    mysql_close(conn);
    return NULL;
}

/* Main */
int main() {
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
