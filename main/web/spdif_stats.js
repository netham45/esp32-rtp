// Auto-refresh control
let autoRefreshInterval = null;
let isAutoRefresh = true;

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    refreshStats();
    if (isAutoRefresh) {
        startAutoRefresh();
    }
});

function startAutoRefresh() {
    if (autoRefreshInterval) {
        clearInterval(autoRefreshInterval);
    }
    autoRefreshInterval = setInterval(refreshStats, 1000);
}

function stopAutoRefresh() {
    if (autoRefreshInterval) {
        clearInterval(autoRefreshInterval);
        autoRefreshInterval = null;
    }
}

function toggleAutoRefresh() {
    const checkbox = document.getElementById('auto-refresh');
    isAutoRefresh = checkbox.checked;
    
    if (isAutoRefresh) {
        startAutoRefresh();
    } else {
        stopAutoRefresh();
    }
}

function formatNumber(num) {
    if (typeof num === 'number') {
        if (num >= 1000000) {
            return (num / 1000000).toFixed(2) + 'M';
        } else if (num >= 1000) {
            return (num / 1000).toFixed(1) + 'K';
        }
    }
    return num.toString();
}

function getErrorTypeName(type) {
    switch(type) {
        case 0: return 'None';
        case 1: return 'BMC Violation';
        case 2: return 'Unexpected Long';
        default: return 'Unknown';
    }
}

function updateStats(data) {
    // Hide loading, show stats
    document.getElementById('loading').style.display = 'none';
    document.getElementById('error-message').style.display = 'none';
    document.getElementById('stats-container').style.display = 'grid';
    
    // Update all stat values
    const fields = [
        'total_symbols_processed', 'empty_symbols',
        'short_pulses', 'medium_pulses', 'long_pulses', 'unknown_pulses',
        'preamble_b_detected', 'preamble_m_detected', 'preamble_w_detected', 'total_preambles',
        'bits_decoded_0', 'bits_decoded_1', 'total_bits_decoded',
        'subframes_completed', 'left_channel_samples', 'right_channel_samples', 'stereo_pairs_output',
        'frames_completed', 'blocks_completed',
        'sync_errors', 'bmc_violations', 'unexpected_long_pulses', 'incomplete_subframes',
        'parity_errors', 'buffer_overruns',
        'sync_acquisitions', 'sync_losses',
        'max_consecutive_errors', 'current_consecutive_errors',
        'last_error_position', 'last_sample_rate', 'timing_updates',
        'processing_cycles', 'max_symbols_per_cycle',
        // RMT statistics
        'rmt_callbacks', 'rmt_symbols_received', 'rmt_buffer_overflows'
    ];
    
    fields.forEach(field => {
        const element = document.getElementById(field);
        if (element && data[field] !== undefined) {
            element.textContent = formatNumber(data[field]);
        }
    });
    
    // Format floating point values
    if (data.avg_symbols_per_cycle !== undefined) {
        document.getElementById('avg_symbols_per_cycle').textContent = 
            data.avg_symbols_per_cycle.toFixed(1);
    }
    
    // Update sync state
    const syncStateElem = document.getElementById('current_sync_state');
    const syncStatusElem = document.getElementById('sync-status');
    const syncIndicator = document.getElementById('sync-indicator');
    
    if (data.current_sync_state === 1) {
        syncStateElem.textContent = 'Synced';
        syncStateElem.className = 'stat-value success-value';
        syncStatusElem.textContent = 'Synced';
        syncIndicator.className = 'status-indicator status-synced';
    } else {
        syncStateElem.textContent = 'Not Synced';
        syncStateElem.className = 'stat-value error-value';
        syncStatusElem.textContent = 'Not Synced';
        syncIndicator.className = 'status-indicator status-not-synced';
    }
    
    // Update error type
    if (data.last_error_type !== undefined) {
        document.getElementById('last_error_type').textContent = 
            getErrorTypeName(data.last_error_type);
    }
    
    // Calculate and display uptime
    if (data.timestamp_start !== undefined && data.timestamp_last_update !== undefined) {
        const uptimeUs = data.timestamp_last_update - data.timestamp_start;
        const uptimeSeconds = Math.floor(uptimeUs / 1000000);
        const hours = Math.floor(uptimeSeconds / 3600);
        const minutes = Math.floor((uptimeSeconds % 3600) / 60);
        const seconds = uptimeSeconds % 60;
        
        let uptimeStr = '';
        if (hours > 0) {
            uptimeStr = `${hours}h ${minutes}m ${seconds}s`;
        } else if (minutes > 0) {
            uptimeStr = `${minutes}m ${seconds}s`;
        } else {
            uptimeStr = `${seconds}s`;
        }
        document.getElementById('uptime').textContent = uptimeStr;
    }
}

function showError(message) {
    document.getElementById('loading').style.display = 'none';
    const errorElem = document.getElementById('error-message');
    errorElem.textContent = 'Error: ' + message;
    errorElem.style.display = 'block';
}

async function refreshStats() {
    try {
        const response = await fetch('/api/spdif/stats');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        const data = await response.json();
        updateStats(data);
    } catch (error) {
        console.error('Error fetching stats:', error);
        showError('Failed to fetch statistics. Make sure SPDIF decoder is active.');
    }
}

async function resetStats() {
    if (!confirm('Are you sure you want to reset all SPDIF decoder statistics?')) {
        return;
    }
    
    try {
        const response = await fetch('/api/spdif/reset', {
            method: 'POST'
        });
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        // Immediately refresh to show reset stats
        refreshStats();
    } catch (error) {
        console.error('Error resetting stats:', error);
        showError('Failed to reset statistics.');
    }
}