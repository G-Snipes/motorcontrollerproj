// client_mysql.js

const mysql = require('mysql2/promise');
const readline = require('readline'); // Allows the ability to write to stream

// Configures the database
const DB_CONFIG = {
    host: '127.0.0.1',
    user: 'motoruser',
    password: 'MotorPass123!', 
    database: 'motordb'
};

let CLIENT_ID = process.argv[2] || 'ClientA'; // Default client name is clientA
const PEER_POLL_INTERVAL_MS = 250; // Check 'commands' table for peer activity every 0.25s

let db;
let last_peer_command_ts = 0; // Timestamp of the last command written to 'commands' table


async function connectDB() { // Connect DB to mysql database
    try {
        db = await mysql.createConnection(DB_CONFIG);
        console.log(`Client ${CLIENT_ID} connected to MySQL database.`);
    } catch (err) {
        console.error('Database connection failed:', err);
        process.exit(1);
    }
}

// Sends command to 'commands' table
async function sendCommand(percentageChange) {

    // SQL query
    const sql = `
        INSERT INTO commands 
        (client_id, percent_change) 
        VALUES (?, ?)
    `;
    
    try {
        // Database automatically sets the 'ts' timestamp
        await db.execute(sql, [CLIENT_ID, percentageChange]);
        console.log(`\n📣 SENT COMMAND: Change by ${percentageChange}% from ${CLIENT_ID}.`);
    } catch (err) {
        console.error('Error sending command:', err.message);
    }
}

// Checks for peer commands from 'commands' table and logs them
async function checkPeerCommands() {
    try {
        // Query for the latest command written by ANY client
        const [rows] = await db.execute(
            `SELECT client_id, percent_change, ts FROM commands ORDER BY ts DESC LIMIT 1`
        );

        if (rows.length > 0) {
            const { client_id, percent_change, ts } = rows[0];
            const commandTime = new Date(ts).getTime();

            if (client_id !== CLIENT_ID && commandTime > last_peer_command_ts) {
                console.log(`\n\n📢 PEER COMMAND DETECTED:`);
                console.log(`  - Issued by: ${client_id}`);
                console.log(`  - Command: Motor speed change by ${percent_change}%`);
                console.log(`  - Time: ${new Date(ts).toLocaleTimeString()}`);
                console.log(`\n[${CLIENT_ID}] Enter command: (e.g., +25, -10, or /setid NewName)`);
                
                last_peer_command_ts = commandTime;
            }
        }
    } catch (err) {
        
        // console.error('Error checking peer commands:', err.message);
    }
}

// Prompt user function
const promptUser = (rlInterface) => {
        rlInterface.question(`\n[${CLIENT_ID}] Enter command: (e.g., +25, -10, or type /setid NewName) `, (answer) => {
            const trimmedAnswer = answer.trim();

            if (trimmedAnswer.startsWith('/setid ')) { // String types after /setid will now be new Client ID
                const newId = trimmedAnswer.substring(7).trim();
                if (newId) {
                    CLIENT_ID = newId;
                    console.log(`\n✅ Client ID successfully changed to: ${CLIENT_ID}`);
                } else {
                    console.log("❌ Invalid /setid command. Usage: /setid NewName");
                }
            } else { // Otherwise detects for new percentage
                const percentage = parseInt(trimmedAnswer);
                if (!isNaN(percentage) && percentage !== 0) {
                    sendCommand(percentage);
                } else {
                    console.log("❌ Invalid input. Please enter a number like +25 or -10.");
                }
            }
            promptUser(rlInterface); // Loops continuously
        });
    };

// main
async function main() {

    // No client ID error checker
    if (!CLIENT_ID) {
        console.error("Please run with a client ID, e.g., 'node client_mysql.js ClientA'");
        process.exit(1);
    }

    await connectDB(); // Connect to database

    setInterval(checkPeerCommands, PEER_POLL_INTERVAL_MS); // Sets up peer monitoring and polls every 0.25s

    // Sets up interactive console for sending commands
    const rl = readline.createInterface({
        input: process.stdin,
        output: process.stdout
    });

    promptUser(rl); // Initial call to promptUser() function loop
    console.log(`Client ${CLIENT_ID} running: Polling peers every ${PEER_POLL_INTERVAL_MS}ms.`);
}

main();