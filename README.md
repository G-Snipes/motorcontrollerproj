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
* **Peer Monitoring:** Each computer reads from the database every **250 ms** to check for commands sent by *other* clients.
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

UML Function Block Diagram:
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

---

## 📈 Technology & Performance Comparison

This project was developed using three configurations to compare **C** vs. **JavaScript (Node.js)** and **mySQL** vs. **PostgreSQL** database performance.

* The project was written in **C** using both mySQL and PostgreSQL drivers.
* It was also written in **JavaScript** using **Node.js** for its **asynchronous, non-blocking I/O model** to manage concurrency.
* **Version Control Note:** A dedicated `js` branch was created for the JavaScript version.

We attempted to compare the performance of these setups, and screen recordings were taken of each running:

| Setup | Description | Video Link |
| :--- | :--- | :--- |
| **PostgreSQL** | General query and setup demonstration. | [PostgreSQL](https://youtu.be/5N355Qy5_Ew) |
| **mySQL in C** | C application connecting to MySQL. | [mySQL in C](https://youtu.be/KW2adEY1mmc) |
| **JS (Node.js)** | JavaScript application connecting to a database. | [JS](https://youtu.be/UgdTlr6xn0s) |

> ℹ️ **Observation:** Ultimately this project proved too simple for a meaningful performance comparison, as all three setups are highly capable of processing this limited data load.

### Conclusion

At this small scale, the performance difference is determined primarily by **network latency**, **thread scheduling**, and the **overhead of the C code**, not the database engine itself.

| Technology | Analysis | Winner |
| :--- | :--- | :--- |
| **Concurrency (C vs. Node.js)** | **C Multithreading** is very fast and efficient (true parallel computing) but requires careful synchronization to prevent issues, adding complexity and overhead. **Node.js**'s asynchronous I/O model was much more straightforward for the code and is ideal for database operations like this. | **Node.js** (for simpler concurrency model and syntax) |
| **Database Query Languages** | **PostgreSQL** is generally more robust and superior for complex queries. **SQL** (mySQL) was perfectly adequate for this simple project, making them nearly equal here. | **PostgreSQL** (for overall long-term robustness) |

