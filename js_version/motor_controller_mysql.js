// motor_controller_mysql.js

const mysql = require('mysql2/promise');

// --- Configuration ---
const DB_CONFIG = {
    host: '127.0.0.1',
    user: 'motoruser',
    password: 'MotorPass123!', 
    database: 'motordb'
};

const WRITE_INTERVAL_MS = 200; // Log to motordb every 200ms
const COMMAND_POLL_INTERVAL_MS = 100; // Check 'commands' table every 100ms
const COMMAND_COOLDOWN_MS = 200; // Ignore new commands for 200ms after one is processed

// --- State Variables ---
let gas_level = 100.0;
let battery_level = 100.0;
let motor_speed = 0.0;  //starts at 0
let motor_speed_set_point = 100.0; 
let motor_temp = 25.0;
let last_command_ts = 0; // Timestamp of the last processed command (from commands table)

// --- PID Control Constants and State ---
const DT = WRITE_INTERVAL_MS / 1000; // Time step in seconds (0.2)
const Kp = 0.5;
const Ki = 0.1;
const Kd = 0.05;
let integral = 0;
let prev_err = 0;

const RANDOM_ERROR_MAX = 0.5;

// Database Connection
let db;

async function connectDB() {
    try {
        db = await mysql.createConnection(DB_CONFIG);
        console.log('Motor Controller connected to MySQL database.');
    } catch (err) {
        console.error('Database connection failed:', err);
        process.exit(1);
    }
}

// --- PID Step ---
function pidStep() {
    const error = motor_speed_set_point - motor_speed; 
    
    // PID Calculations
    const proportionalOutput = Kp * error;  
    integral += error * DT;                 
    const integralOutput = Ki * integral;   
    const derivative = (error - prev_err) / DT;  
    const derivativeOutput = Kd * derivative;    
    const total = proportionalOutput + integralOutput + derivativeOutput; 

    // Update Speed and State
    const randomError = (Math.random() * 2 * RANDOM_ERROR_MAX) - RANDOM_ERROR_MAX;  
    motor_speed += total + randomError;  

    prev_err = error; 

    motor_speed = Math.max(0, Math.min(100, motor_speed));
    motor_temp = 25.0 + (motor_speed * 0.2);
    // Gas/Battery simple decay (optional, but makes simulation more complete)
    gas_level = Math.max(0, gas_level - 0.01 * motor_speed * DT); 
    battery_level = Math.max(0, battery_level - 0.05 * motor_speed * DT); 
}

// --- Write Motor Data to Log (motordb) ---
async function writeData() {
    // [DEBUG] writeData running... 

    // 1. Update State Values (MUST be first)
    pidStep(); 
    
    const sql = `
        INSERT INTO motordb 
        (gas_level, battery_level, motor_speed, motor_speed_set_point, motor_temp)
        VALUES (?, ?, ?, ?, ?)
    `;
    
    try {
        // 2. Execute the Database Insert
        await db.execute(sql, [
            gas_level, 
            battery_level, 
            motor_speed, 
            motor_speed_set_point, 
            motor_temp
        ]);
        
        // 3. Log the Success/Live Data (MUST be after the DB execute)
        console.log(
            `[LOG] Speed: ${motor_speed.toFixed(2).padStart(6)} | ` +
            `SetPt: ${motor_speed_set_point.toFixed(2).padStart(6)} | ` +
            `Temp: ${motor_temp.toFixed(2).padStart(6)} | ` +
            `Gas: ${gas_level.toFixed(1).padStart(4)}% | ` +
            `Battery: ${battery_level.toFixed(1).padStart(5)}%`
        );
        
    } catch (err) {
        // 4. Log the Failure (If we reach here, the INSERT failed)
        // This is the error message we need to see if the table is still wrong.
        console.error('!!! CRITICAL DB WRITE ERROR !!! Check table name or columns.', err.message); 
    }
}

// --- Check for New Commands from Clients (commmandsJS table) ---
async function checkCommands() { 
    try {
        // Find the LATEST command written by ANY client
        const [rows] = await db.execute(
            `SELECT percent_change, ts FROM commandsJS ORDER BY ts DESC LIMIT 1`
        );

        if (rows.length > 0) {
            const commandTime = new Date(rows[0].ts).getTime();
            const percentChange = rows[0].percent_change;

            if (commandTime > last_command_ts + COMMAND_COOLDOWN_MS) {
                
                // 1. Calculate the new setpoint based on the last known speed (from PID state)
                const currentSetpoint = motor_speed_set_point;
                const changeAmount = currentSetpoint * (percentChange / 100.0);
                const newSetpoint = Math.max(0, Math.min(100, currentSetpoint + changeAmount));
                
                // 2. Apply and Log the Command
                motor_speed_set_point = parseFloat(newSetpoint);
                last_command_ts = commandTime; 
                console.log(`\n✅ COMMAND RECEIVED: New Setpoint is ${motor_speed_set_point.toFixed(2)}.`);

                // Reset PID State for smooth response
                integral = 0; 
                prev_err = 0; 
            }
        }
    } catch (err) {
        console.error('Error reading commands:', err.message);
    }
}

// --- Main Execution ---
async function main() {
    await connectDB(); //connect to SQL server
    
    // Set up the periodic tasks
    setInterval(writeData, WRITE_INTERVAL_MS);
    setInterval(checkCommands, COMMAND_POLL_INTERVAL_MS);
    
    console.log(`Motor Controller running: Logging to motordb every ${WRITE_INTERVAL_MS}ms, Polling 'commands' every ${COMMAND_POLL_INTERVAL_MS}ms.`);
}

main();