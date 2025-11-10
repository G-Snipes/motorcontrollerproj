# ⚙️ Motor Controller Simulator Project Using PID

## Project Summary

This project simulates a real-world system where multiple clients issue commands to a single motor controller through a database.

This project simulates a real-world system where multiple clients issue commands to a single motor controller through a database.
One executable simulates the motor controller writing to a database.
Another executable will allow a client to send commands to the motor controller. 
(To simulate multiple clients sending commands to the motor simultaneously, this executable can be run on separate network ports.)
The executables communicate via a shared data stream.


---

## 🔬 Functional Requirements

### Motor Controller

The controller reads commands, updates the set point, and logs real-time motor data.

| Action | Frequency/Mechanism | Data/Details |
| :--- | :--- | :--- |
| **Data Logging** | Writes to the live data stream every **200 ms** (at least). | Gas tank level, Battery level, Motor speed, Motor speed set point, Motor temperature. |
| **Command Check** | Reads from the shared database every **100 ms** to check for commands. | Commands update the motor speed set point. |
| **Simulation** | Motor speed uses a simple **PID loop** with random error introduced to allow for simulated overshoot and undershoot. |
| **Throttling** | Ignores any new commands within **200 ms** after processing a command. |

### Client Commands

Clients issue percentage-based speed change commands and monitor peer activity.

* **Command Issuance:** Clients send commands (via the data stream) to change the motor speed by a given percentage.
* **Peer Monitoring:** Each computer reads from the database every **250 ms** to check for commands sent from other clients.
* **Example:** When Computer A reads that Computer B has given the command "Motor speed increase by 25%," it displays the command and the identity of the issuer.

---

## 🛠️ Installation for Mac

### MySQL Setup Commands

```bash
# Install brew if you haven’t
/bin/bash -c "$(curl -fsSL [https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh](https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh))"
# Install and start MySQL
brew install mysql
brew services start mysql
# Secure the installation (set a password)
mysql_secure_installation
# Log in
mysql -u root -p
```

### PostgreSQL Setup Commands

```bash
brew install libpq
brew install postgresql
brew services start postgresql@14
createuser motoruser --createdb --login --pwprompt
# Enter the password: MotorPass123! (Matches C code expectation)

createdb motordb --owner=motoruser
gcc motor_controller_pgsql.c -o motor_controller_pgsql -lpq -lpthread -lm
```

### UML Function Block Diagram:
<img width="616" height="442" alt="Screenshot 2025-11-09 at 7 21 22 PM" src="https://github.com/user-attachments/assets/e7defb72-9e1e-425e-bf12-24e7af1907f2" />

## 🗃️ Database Structure

### Telemetry Table

This table stores the motor's live operational data.

| Column | Data Type |
| :--- | :--- |
| `id` | `int` |
| `ts` | `timestamp` |
| `gas_level` | `double` |
| `battery_level` | `double` |
| `motor_speed` | `double` |
| `motor_speed_set_point` | `double` |
| `motor_temp` | `double` |

### Commands Table

This table stores the commands issued by clients.

| Column | Data Type |
| :--- | :--- |
| `id` | `int` |
| `ts` | `timestamp` |
| `client_id` | `string` |
| `percent_change` | `double` |
| `issued_via` | `string` |
| `processed` | `int` |
| `processed_ts` | `timestamp` |
| `processed_by` | `string` |

*For version control I just made a seperate js branch.
---

## 📈 Technology & Performance Comparison

This project was written in C and used mySQL and PostgreSQL as I wanted to compare and try out the two. I also wrote it in Javascript and used Node.js for its asynchronous, non-blocking I/O model for concurrency. 

I tried my best to compare the performances between the two languages and between mySQL and PostgreSQL query languages. I took screen recordings below of each of them running. 


Ultimately this project is a bit too simple to compare the performances of these three setups as they are all highly capable of processing this much data. 

Below are screen recordings of each setup running and the simlutated values in real time:

| Setup | Video Link |
| :--- | :--- |
| **PostgreSQL** | [PostgreSQL](https://youtu.be/5N355Qy5_Ew) |
| **mySQL in C** | [mySQL in C](https://youtu.be/KW2adEY1mmc) |
| **JS (Node.js)** | [JS](https://youtu.be/UgdTlr6xn0s) |

> ℹ️ **Observation:** Ultimately this project proved too simple for a meaningful performance comparison, as all three setups are highly capable of processing this limited data load.

### Conclusion

At this small scale, the performance difference is determined by network latency, thread scheduling, and the overhead of the C code, but not really the database engine itself.

Multithreading in C can be very fast and efficient but it requires careful synchronization to prevent race conditions or memory issues. This is true parallel computing and is great for CPU intensive tasks however it is more complex (especially with its  syntax) and there is more overhead. 

Using Node.js for its asynchronous I/O single-threaded event loop structure was much more straightforward when it came to the code. It's great for database operations and a simple project like this one but it would need worker threads for more CPU-intensive tasks. Node.js was the winner for this project for its simpler concurrency model and syntax. 

As for the database querying languages, PostgreSQL is likely the more superior one as it is just simply more robust. SQL was able to handle this project perfectly fine, and these two languages are pretty equal unless you really start pushing their limits query-wise. In which case, PostgreSQL is the winner.

