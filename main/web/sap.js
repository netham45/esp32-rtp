// SAP Announcements JavaScript

// Configuration
const REFRESH_INTERVAL = 5000; // 5 seconds
const API_ENDPOINT = '/api/sap';
const CONFIG_ENDPOINT = '/api/settings';

// State
let refreshTimer = null;
let currentStreamName = '';
let isConnected = false;

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    loadCurrentConfig();
    loadAnnouncements();

    // Setup event listeners
    setupEventListeners();

    // Start auto-refresh
    refreshTimer = setInterval(() => {
        loadCurrentConfig();
        loadAnnouncements();
    }, REFRESH_INTERVAL);
});

// Setup event listeners
function setupEventListeners() {
    const stopBtn = document.getElementById('stop-listening-btn');
    if (stopBtn) {
        stopBtn.addEventListener('click', stopListening);
    }
}

// Load announcements from API
async function loadAnnouncements() {
    const loadingElement = document.getElementById('loading');
    const contentElement = document.getElementById('announcement-content');
    const errorElement = document.getElementById('error-message');
    const lastUpdateElement = document.getElementById('last-update');
    
    try {
        const response = await fetch(API_ENDPOINT);
        
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        
        const data = await response.json();
        
        // Hide loading and error messages
        loadingElement.style.display = 'none';
        errorElement.style.display = 'none';
        contentElement.style.display = 'block';
        
        // Update last refresh time
        const now = new Date();
        lastUpdateElement.textContent = `Last updated: ${now.toLocaleTimeString()}`;
        
        // Display announcements
        displayAnnouncements(data.announcements);

        // Update listening status
        updateListeningStatus();
        
    } catch (error) {
        console.error('Error fetching SAP announcements:', error);
        
        // Show error message
        errorElement.textContent = `Failed to load SAP announcements: ${error.message}`;
        errorElement.style.display = 'block';
        loadingElement.style.display = 'none';
        contentElement.style.display = 'none';
    }
}

// Display announcements in table
function displayAnnouncements(announcements) {
    const tbody = document.getElementById('announcement-tbody');
    const noDataElement = document.getElementById('no-data');
    const tableElement = document.getElementById('announcement-table');
    
    // Clear existing rows
    tbody.innerHTML = '';
    
    if (!announcements || announcements.length === 0) {
        // Show no data message
        tableElement.style.display = 'none';
        noDataElement.style.display = 'block';
        return;
    }
    
    // Hide no data message and show table
    noDataElement.style.display = 'none';
    tableElement.style.display = 'table';
    
    // Sort announcements by active status first, then by last seen
    announcements.sort((a, b) => {
        if (a.active !== b.active) {
            return b.active ? 1 : -1; // Active first
        }
        return a.last_seen_ago - b.last_seen_ago; // Most recent first
    });
    
    // Add rows for each announcement
    announcements.forEach(announcement => {
        const row = tbody.insertRow();
        
        // Status indicator
        const statusCell = row.insertCell();
        const statusIndicator = document.createElement('span');
        statusIndicator.className = `status ${announcement.active ? 'active' : 'inactive'}`;
        statusIndicator.title = announcement.active ? 'Active' : 'Inactive';
        statusCell.appendChild(statusIndicator);
        const statusText = document.createElement('span');
        statusText.textContent = announcement.active ? 'Active' : 'Inactive';
        statusCell.appendChild(statusText);
        
        // Stream name
        const nameCell = row.insertCell();
        nameCell.textContent = announcement.stream_name || 'Unknown';
        
        // Source IP (sender of SAP announcement)
        const sourceIpCell = row.insertCell();
        // Check for empty string as well as null/undefined
        sourceIpCell.textContent = (announcement.source_ip && announcement.source_ip.trim()) ? announcement.source_ip : 'N/A';
        
        // Destination IP (multicast group)
        const destIpCell = row.insertCell();
        // Use destination_ip from backend if available, otherwise try to extract from session_info
        let destIp = 'N/A';
        if (announcement.destination_ip && announcement.destination_ip !== 'N/A') {
            destIp = announcement.destination_ip;
        } else if (announcement.session_info) {
            const match = announcement.session_info.match(/Connection:\s*(\d+\.\d+\.\d+\.\d+)/);
            if (match) {
                destIp = match[1];
            }
        }
        destIpCell.textContent = destIp;
        
        // Port
        const portCell = row.insertCell();
        portCell.textContent = announcement.port || 'N/A';
        
        // Sample rate
        const sampleRateCell = row.insertCell();
        if (announcement.sample_rate) {
            sampleRateCell.textContent = `${announcement.sample_rate.toLocaleString()} Hz`;
        } else {
            sampleRateCell.textContent = 'N/A';
        }

        // Actions
        const actionsCell = row.insertCell();
        if (announcement.active) {
            if (currentStreamName === announcement.stream_name) {
                actionsCell.innerHTML = '<span class="listening-indicator">LISTENING</span>';
            } else {
                const listenBtn = document.createElement('button');
                listenBtn.className = 'listen-btn';
                listenBtn.textContent = 'Listen';
                listenBtn.onclick = () => listenToStream(announcement.stream_name);
                actionsCell.appendChild(listenBtn);
            }
        } else {
            actionsCell.textContent = '-';
        }
        
        // Last seen
        const lastSeenCell = row.insertCell();
        if (announcement.last_seen_ago !== undefined) {
            lastSeenCell.innerHTML = formatTimeAgo(announcement.last_seen_ago);
        } else {
            lastSeenCell.textContent = 'Unknown';
        }
        
        // Add row styling based on status
        if (!announcement.active) {
            row.style.opacity = '0.6';
        }
    });
}

// Format time ago display
function formatTimeAgo(seconds) {
    if (seconds < 60) {
        return `<span class="time-ago">${seconds}s ago</span>`;
    } else if (seconds < 3600) {
        const minutes = Math.floor(seconds / 60);
        return `<span class="time-ago">${minutes}m ago</span>`;
    } else if (seconds < 86400) {
        const hours = Math.floor(seconds / 3600);
        return `<span class="time-ago">${hours}h ago</span>`;
    } else {
        const days = Math.floor(seconds / 86400);
        return `<span class="time-ago">${days}d ago</span>`;
    }
}

// Load current configuration
async function loadCurrentConfig() {
    try {
        const response = await fetch(CONFIG_ENDPOINT);
        if (response.ok) {
            const config = await response.json();
            currentStreamName = config.sap_stream_name || '';
            // Consider connected if we have a stream name configured
            // Since the backend handles the connection automatically
            isConnected = (currentStreamName && currentStreamName.length > 0);
        }
    } catch (error) {
        console.error('Error loading current config:', error);
    }
}

// Update listening status display
function updateListeningStatus() {
    const statusElement = document.getElementById('listening-status');
    const currentStreamElement = document.getElementById('current-stream');
    const connectionStatusElement = document.getElementById('connection-status');

    if (currentStreamName && currentStreamName.length > 0) {
        statusElement.style.display = 'block';
        currentStreamElement.textContent = currentStreamName;
        // Show as configured since backend handles the actual connection
        connectionStatusElement.textContent = 'Configured';
        connectionStatusElement.style.color = '#4caf50';
    } else {
        statusElement.style.display = 'none';
    }
}

// Start listening to a stream
async function listenToStream(streamName) {
    try {
        const response = await fetch(CONFIG_ENDPOINT, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                sap_stream_name: streamName
            })
        });

        if (response.ok) {
            currentStreamName = streamName;
            isConnected = false; // Will update on next refresh
            updateListeningStatus();
            console.log(`Started listening to stream: ${streamName}`);
        } else {
            throw new Error(`Failed to set stream: ${response.status}`);
        }
    } catch (error) {
        console.error('Error starting to listen to stream:', error);
        alert(`Failed to start listening to stream: ${error.message}`);
    }
}

// Stop listening to current stream
async function stopListening() {
    try {
        const response = await fetch(CONFIG_ENDPOINT, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                sap_stream_name: ''
            })
        });

        if (response.ok) {
            currentStreamName = '';
            isConnected = false;
            updateListeningStatus();
            console.log('Stopped listening to stream');
        } else {
            throw new Error(`Failed to stop listening: ${response.status}`);
        }
    } catch (error) {
        console.error('Error stopping stream:', error);
        alert(`Failed to stop listening: ${error.message}`);
    }
}

// Clean up on page unload
window.addEventListener('beforeunload', function() {
    if (refreshTimer) {
        clearInterval(refreshTimer);
    }
});