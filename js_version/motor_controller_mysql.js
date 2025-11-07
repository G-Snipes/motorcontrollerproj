// motor_controller_mysql.js

const mysql = require('mysql2/promise'); //instantiates sql database module for asynchronous computing

// configures the database (same as c version)
const DB_CONFIG = {
    host: '127.0.0.1',
    user: 'motoruser',
    password: 'MotorPass123!', 
    database: 'motordb'
};

const WRITE_INTERVAL_MS = 200; // Log to motordb every 200ms
const COMMAND_POLL_INTERVAL_MS = 100; // Check 'commands' table every 100ms
const COMMAND_COOLDOWN_MS = 200; // Ignore new commands for 200ms after one is processed

// State variables
let gas_level = 100.0;
let battery_level = 100.0;
let motor_speed = 0.0;  //starts at 0
let motor_speed_set_point = 100.0; //starts at 100 (speed can't exceed 100)
let motor_temp = 25.0;
let last_command_ts = 0; // Timestamp of the last processed command (from commands table)

// PID Control Constants
const DT = WRITE_INTERVAL_MS / 1000; // Time step in seconds (0.2)
const Kp = 0.5;
const Ki = 0.1;
const Kd = 0.05;
let integral = 0;
let prev_err = 0;

const RANDOM_ERROR_MAX = 0.5;

// Database Connection
let db;

async function connectDB() { // Connect db to mysql database
    try {
        db = await mysql.createConnection(DB_CONFIG); 
        console.log('Motor Controller connected to MySQL database.');
    } catch (err) {
        console.error('Database connection failed:', err);
        process.exit(1);
    }
}

// Will stop the motor controller if gas or battery are depleted
function depletedLevels() {

    if ((gas_level <= 0) || (battery_level <= 0)) { //motor speed goes to 0 if gas or battery are depleted.
        // First time hitting zero, trigger shutdown
        return true;
    }

    return false;
}

// Performs pid calculations
function pidStep() {
    const error = motor_speed_set_point - motor_speed; //error equals the desired set point - current motor speed
    
    if (depletedLevels() == true) {
        motor_speed_set_point = 0.0; // Force setpoint to zero
        prev_err = 0;
        console.error('\n🚨 CRITICAL SHUTDOWN: Gas or Battery Exhausted! Setting setpoint to 0.0.');
    }

    // PID Calculations
    const proportionalOutput = Kp * error;  
    integral += error * DT;                 
    const integralOutput = Ki * integral;   
    const derivative = (error - prev_err) / DT;  
    const derivativeOutput = Kd * derivative;    
    const total = proportionalOutput + integralOutput + derivativeOutput; 

    // Updates speed and applies random error
    const randomError = (Math.random() * 2 * RANDOM_ERROR_MAX) - RANDOM_ERROR_MAX; 
    motor_speed += total + randomError;  

    prev_err = error; //update with new error

    motor_speed = Math.max(0, Math.min(100, motor_speed)); // *Ensures motor speed does stays inbetween 0 - 100

    motor_temp = 25.0 + (motor_speed * 0.2); //simulates motor temp
    gas_level = Math.max(0, gas_level - 0.01 * motor_speed * DT); //simulates gas_level
    battery_level = Math.max(0, battery_level - 0.001 * motor_speed * DT); //simulates battery level
}

// Write motor data to log 
async function writeData() {

    pidStep(); // Update state values

    // Prepares SQL statement for inserting new values into database
    const sql = `
        INSERT INTO motordb 
        (gas_level, battery_level, motor_speed, motor_speed_set_point, motor_temp)
        VALUES (?, ?, ?, ?, ?)
    `;
    
    try {
        // Executes database insert
        await db.execute(sql, [
            gas_level, 
            battery_level, 
            motor_speed, 
            motor_speed_set_point, 
            motor_temp
        ]);
        
        // Logs the success/live data
        console.log(
            `[LOG] Speed: ${motor_speed.toFixed(2).padStart(6)} | ` +
            `SetPt: ${motor_speed_set_point.toFixed(2).padStart(6)} | ` +
            `Temp: ${motor_temp.toFixed(2).padStart(6)} | ` +
            `Gas: ${gas_level.toFixed(1).padStart(4)}% | ` +
            `Battery: ${battery_level.toFixed(1).padStart(5)}%`
        );
        
    } catch (err) { // Failure log
        console.error('!!! CRITICAL DB WRITE ERROR !!! Check table name or columns.', err.message); 
    }
}

// Polls client for new command
async function checkCommands() { 
    try {
        // Find the latest command written by any client
        const [rows] = await db.execute(
            `SELECT percent_change, ts FROM commands ORDER BY ts DESC LIMIT 1`
        );

        if (rows.length > 0) {
            const commandTime = new Date(rows[0].ts).getTime();
            const percentChange = rows[0].percent_change;

            if (commandTime > last_command_ts + COMMAND_COOLDOWN_MS) {
                
                // Calculates the new motor speed set point based on the last known speed from the PID state
                const currentSetpoint = motor_speed_set_point;
                const changeAmount = currentSetpoint * (percentChange / 100.0);
                const newSetpoint = Math.max(0, Math.min(100, currentSetpoint + changeAmount)); // sets it between 0 - 100
                
                // apply and log command
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

// main
async function main() {
    await connectDB(); // Connect to SQL server
    
    setInterval(writeData, WRITE_INTERVAL_MS); // Writes to database every 0.2s
    setInterval(checkCommands, COMMAND_POLL_INTERVAL_MS); // Polls for commands every 0.1s 
    
    console.log(`Motor Controller running: Logging to motordb every ${WRITE_INTERVAL_MS}ms, Polling 'commands' every ${COMMAND_POLL_INTERVAL_MS}ms.`);
}

main();