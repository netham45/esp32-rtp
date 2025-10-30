// Enhanced ESP32 Web Interface Script with Modern Interactivity & Mobile Support

// ===== Constants & Configuration =====
const DEBOUNCE_DELAY = 300;
const NETWORK_SCAN_COOLDOWN = 10000; // 10 seconds
const TOAST_DURATION = 3000;
const RETRY_DELAYS = [1000, 2000, 4000, 8000]; // Exponential backoff
const SWIPE_THRESHOLD = 50;
const PULL_THRESHOLD = 80;
const AUTO_REFRESH_INTERVAL = 5000; // 5 seconds for auto-refresh

// ===== State Management =====
const state = {
    activeTab: 'advanced-tab',
    unsavedChanges: new Set(),
    requestQueue: [],
    isProcessingQueue: false,
    originalSettings: {},
    domCache: new Map(),
    isMobile: false,
    touchStartX: 0,
    touchStartY: 0
};

// ===== DOM Cache Helper =====
function $(selector) {
    if (!state.domCache.has(selector)) {
        state.domCache.set(selector, document.querySelector(selector));
    }
    return state.domCache.get(selector);
}

function $$(selector) {
    return document.querySelectorAll(selector);
}

// ===== Initialization =====
// ===== Mobile Detection =====
function detectMobile() {
    const userAgent = navigator.userAgent || navigator.vendor || window.opera;
    const mobileRegex = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i;
    const isTouchDevice = 'ontouchstart' in window || navigator.maxTouchPoints > 0;
    const isSmallScreen = window.matchMedia('(max-width: 768px)').matches;
    
    return mobileRegex.test(userAgent) || (isTouchDevice && isSmallScreen);
}

// ===== Mobile Initialization =====
function initializeMobileFeatures() {
    state.isMobile = detectMobile();
    
    if (state.isMobile) {
        document.body.classList.add('is-mobile');
        initializeTouchHandlers();
        initializePullToRefresh();
        initializeMobileMenu();
        initializeFloatingActionButton();
        optimizeForMobile();
    }
    
    // Re-detect on orientation change
    window.addEventListener('orientationchange', () => {
        setTimeout(() => {
            state.isMobile = detectMobile();
            if (state.isMobile) {
                document.body.classList.add('is-mobile');
            } else {
                document.body.classList.remove('is-mobile');
            }
        }, 100);
    });
}

// ===== Touch Handlers =====
function initializeTouchHandlers() {
    let touchEndX = 0;
    
    document.addEventListener('touchstart', handleTouchStart, { passive: true });
    document.addEventListener('touchend', handleTouchEnd, { passive: true });
    
    function handleTouchStart(e) {
        state.touchStartX = e.changedTouches[0].screenX;
        state.touchStartY = e.changedTouches[0].screenY;
    }
    
    function handleTouchEnd(e) {
        touchEndX = e.changedTouches[0].screenX;
        const touchEndY = e.changedTouches[0].screenY;
        
        const deltaX = touchEndX - state.touchStartX;
        const deltaY = touchEndY - state.touchStartY;
        
        // Only process horizontal swipes
        if (Math.abs(deltaX) > Math.abs(deltaY) && Math.abs(deltaX) > SWIPE_THRESHOLD) {
            handleSwipeGesture(deltaX);
        }
    }
}

// ===== Swipe Navigation =====
function handleSwipeGesture(deltaX) {
    const tabs = ['advanced-tab', 'logs-tab', 'ota-tab'];
    const currentIndex = tabs.indexOf(state.activeTab);
    
    if (deltaX > 0 && currentIndex > 0) {
        // Swipe right - go to previous tab
        switchTab(tabs[currentIndex - 1]);
        provideHapticFeedback();
    } else if (deltaX < 0 && currentIndex < tabs.length - 1) {
        // Swipe left - go to next tab
        switchTab(tabs[currentIndex + 1]);
        provideHapticFeedback();
    }
}

// ===== Mobile Menu =====
function initializeMobileMenu() {
    const menuToggle = $('#mobileMenuToggle');
    const tabsNav = $('#tabsNav');
    const mainContainer = $('#mainContainer');
    
    if (!menuToggle || !tabsNav) return;
    
    menuToggle.addEventListener('click', (e) => {
        e.stopPropagation();
        const isOpen = menuToggle.classList.contains('active');
        
        if (isOpen) {
            closeMobileMenu();
        } else {
            openMobileMenu();
        }
        
        provideHapticFeedback();
    });
    
    // Close menu when clicking outside (on overlay)
    document.addEventListener('click', (e) => {
        if (menuToggle.classList.contains('active') &&
            !tabsNav.contains(e.target) &&
            !menuToggle.contains(e.target)) {
            closeMobileMenu();
        }
    });
    
    // Close menu when selecting a tab
    $$('.tab').forEach(tab => {
        tab.addEventListener('click', () => {
            if (state.isMobile) {
                setTimeout(closeMobileMenu, 100);
            }
        });
    });
}

function openMobileMenu() {
    const menuToggle = $('#mobileMenuToggle');
    const tabsNav = $('#tabsNav');
    
    // Use existing overlay element
    const overlay = document.getElementById('mobile-menu-overlay');
    
    menuToggle.classList.add('active');
    tabsNav.classList.add('active');
    if (overlay) overlay.style.display = 'block';
}

function closeMobileMenu() {
    const menuToggle = $('#mobileMenuToggle');
    const tabsNav = $('#tabsNav');
    const overlay = document.getElementById('mobile-menu-overlay');
    
    menuToggle.classList.remove('active');
    tabsNav.classList.remove('active');
    if (overlay) {
        overlay.style.display = 'none';
    }
}

// ===== Floating Action Button =====
function initializeFloatingActionButton() {
    const fab = $('#floatingActionButton');
    if (!fab) return;
    
    fab.addEventListener('click', () => {
        handleFABAction();
        provideHapticFeedback();
    });
    
    updateFABIcon();
}

function handleFABAction() {
    // FAB is no longer needed with simplified interface
    const fab = $('#floatingActionButton');
    if (fab) {
        fab.style.display = 'none';
    }
}

function updateFABIcon() {
    // FAB is no longer needed with simplified interface
    const fab = $('#floatingActionButton');
    if (fab) {
        fab.style.display = 'none';
    }
}

// ===== Mobile Optimizations =====
function optimizeForMobile() {
    // Optimize input focus behavior
    $$('input, select, textarea').forEach(element => {
        element.addEventListener('focus', () => {
            if (state.isMobile) {
                setTimeout(() => {
                    element.scrollIntoView({
                        behavior: 'smooth',
                        block: 'center'
                    });
                }, 300);
            }
        });
    });
    
    // Add touch feedback to buttons
    $$('button, .tab').forEach(element => {
        element.addEventListener('touchstart', () => {
            element.classList.add('touch-active');
        }, { passive: true });
        
        element.addEventListener('touchend', () => {
            setTimeout(() => {
                element.classList.remove('touch-active');
            }, 100);
        }, { passive: true });
    });
    
    // Prevent double-tap zoom
    let lastTouchEnd = 0;
    document.addEventListener('touchend', (e) => {
        const now = Date.now();
        if (now - lastTouchEnd <= 300) {
            e.preventDefault();
        }
        lastTouchEnd = now;
    }, false);
    
    // Handle keyboard visibility
    if (state.isMobile) {
        window.addEventListener('resize', handleKeyboardVisibility);
    }
}

function handleKeyboardVisibility() {
    const windowHeight = window.innerHeight;
    const documentHeight = document.documentElement.clientHeight;
    
    if (windowHeight < documentHeight * 0.75) {
        // Keyboard is likely visible
        document.body.classList.add('keyboard-visible');
    } else {
        // Keyboard is likely hidden
        document.body.classList.remove('keyboard-visible');
    }
}

// ===== Haptic Feedback =====
function provideHapticFeedback(duration = 10) {
    if ('vibrate' in navigator) {
        navigator.vibrate(duration);
    }
}

document.addEventListener('DOMContentLoaded', function() {
    // Initialize loading system FIRST (before anything that might show loading)
    initializeLoadingSystem();
    
    // Initialize mobile features
    initializeMobileFeatures();
    
    initializeTabSystem();
    initializeToastSystem();
    
    // Initialize advanced features
    initializeExportImport();
    initializeHelpTooltips();
    initializeKeyboardShortcuts();
    
    restoreTabState();
});

// ===== Tab System with Hash Support =====
function initializeTabSystem() {
    const tabs = $$('.tab');
    const contents = $$('.tab-content');
    
    // Update tab click handlers
    tabs.forEach(tab => {
        if (tab.id === 'wizard-tab') {
            // Special handling for wizard tab, which is not a real tab
            tab.addEventListener('click', (e) => {
                e.preventDefault();
                showSetupWizard();
            });
            return;
        }
        tab.removeAttribute('onclick'); // Remove inline onclick
        tab.addEventListener('click', (e) => {
            let tabName;
            const text = tab.textContent.toLowerCase();
            if (text === 'import/export') {
                tabName = 'advanced-tab';
            } else if (text === 'system update') {
                tabName = 'ota-tab';
            } else {
                tabName = text.replace(' ', '-') + '-tab';
            }
            switchTab(tabName);
        });
        
        // Add data-tab attribute
        let tabName;
        const text = tab.textContent.toLowerCase();
        if (text === 'import/export') {
            tabName = 'advanced-tab';
        } else if (text === 'system update') {
            tabName = 'ota-tab';
        } else {
            tabName = text.replace(' ', '-') + '-tab';
        }
        tab.dataset.tab = tabName;
    });
    
    // Handle hash changes
    window.addEventListener('hashchange', handleHashChange);
    
    // Keyboard navigation
    document.addEventListener('keydown', (e) => {
        if (e.target.matches('input, textarea, select')) return;
        
        const currentIndex = Array.from(tabs).findIndex(t => t.classList.contains('active'));
        let newIndex = currentIndex;
        
        if (e.key === 'ArrowRight') {
            newIndex = (currentIndex + 1) % tabs.length;
        } else if (e.key === 'ArrowLeft') {
            newIndex = (currentIndex - 1 + tabs.length) % tabs.length;
        } else {
            return;
        }
        
        e.preventDefault();
        const text = tabs[newIndex].textContent.toLowerCase();
        let tabName;
        if (text === 'import/export') {
            tabName = 'advanced-tab';
        } else if (text === 'system update') {
            tabName = 'ota-tab';
        } else {
            tabName = text.replace(' ', '-') + '-tab';
        }
        switchTab(tabName);
    });
    
    // Initial hash check
    handleHashChange();
}

function switchTab(tabName, skipAnimation = false) {
    const tabs = $$('.tab');
    const contents = $$('.tab-content');
    
    // Map hash to tab name
    const hashMap = {
        'advanced': 'advanced-tab',
        'import/export': 'advanced-tab',
        'logs': 'logs-tab',
        'ota': 'ota-tab',
        'system-update': 'ota-tab'
    };
    
    // If tabName is a hash value, convert it
    if (hashMap[tabName]) {
        tabName = hashMap[tabName];
    }
    
    tabs.forEach(tab => {
        let currentTabName;
        const text = tab.textContent.toLowerCase();
        if (text === 'import/export') {
            currentTabName = 'advanced-tab';
        } else if (text === 'system update') {
            currentTabName = 'ota-tab';
        } else {
            currentTabName = text.replace(' ', '-') + '-tab';
        }
        tab.classList.toggle('active', currentTabName === tabName);
    });
    
    contents.forEach(content => {
        content.classList.toggle('active', content.id === tabName);
    });
    
    state.activeTab = tabName;
    const hash = tabName.replace('-tab', '');
    window.location.hash = hash;
    localStorage.setItem('activeTab', tabName);
    
    // Scroll to top on tab change
    if (!skipAnimation) {
        window.scrollTo({ top: 0, behavior: 'smooth' });
    }
}

function handleHashChange() {
    const hash = window.location.hash.slice(1);
    const validHashes = ['advanced', 'import/export', 'logs', 'ota'];
    
    if (validHashes.includes(hash)) {
        if (hash === 'ota') {
            switchTab('ota-tab');
        } else if (hash === 'import/export') {
            switchTab('advanced-tab');
        } else {
            switchTab(hash + '-tab');
        }
    }
}

function restoreTabState() {
    const savedTab = localStorage.getItem('activeTab');
    const hash = window.location.hash.slice(1);
    const validHashes = ['advanced', 'import/export', 'logs', 'ota'];
    
    if (validHashes.includes(hash)) {
        if (hash === 'ota') {
            switchTab('ota-tab');
        } else if (hash === 'import/export') {
            switchTab('advanced-tab');
        } else {
            switchTab(hash + '-tab');
        }
    } else if (savedTab && ['advanced-tab', 'logs-tab', 'ota-tab'].includes(savedTab)) {
        switchTab(savedTab);
    } else {
        switchTab('advanced-tab');
    }
}


// ===== Loading States System =====
function initializeLoadingSystem() {
    // Loading overlay is now in HTML, no need to create
    // Just verify it exists
    const overlay = document.getElementById('loading-overlay');
    if (!overlay) {
        console.warn('Loading overlay not found in HTML');
    }
}

function showLoading(text = 'Loading...') {
    let overlay = document.getElementById('loading-overlay');
    
    // If overlay doesn't exist, create it
    if (!overlay) {
        console.warn('Loading overlay not found, initializing...');
        initializeLoadingSystem();
        overlay = document.getElementById('loading-overlay');
    }
    
    if (!overlay) {
        console.error('Failed to create loading overlay');
        return;
    }
    
    const loadingText = overlay.querySelector('.loading-text');
    if (loadingText) {
        loadingText.textContent = text;
    }
    overlay.classList.add('active');
}

function hideLoading() {
    const overlay = document.getElementById('loading-overlay');
    if (overlay) {
        overlay.classList.remove('active');
    }
}

function setButtonLoading(button, isLoading) {
    if (!button) return;
    
    if (isLoading) {
        button.disabled = true;
        button.dataset.originalText = button.textContent;
        button.innerHTML = '<span class="spinner-small"></span> Processing...';
        button.classList.add('loading');
    } else {
        button.disabled = false;
        button.textContent = button.dataset.originalText || button.textContent;
        button.classList.remove('loading');
    }
}

// ===== Toast Notification System =====
function initializeToastSystem() {
    // Toast container is now in HTML, just verify it exists
    const container = document.getElementById('toast-container');
    if (!container) {
        console.warn('Toast container not found in HTML');
    }
}

function showToast(message, type = 'info', duration = TOAST_DURATION) {
    const container = $('#toast-container');
    
    // Provide haptic feedback on mobile
    if (state.isMobile) {
        if (type === 'success') {
            provideHapticFeedback([50, 30, 50]);
        } else if (type === 'error') {
            provideHapticFeedback([100, 50, 100]);
        } else {
            provideHapticFeedback(20);
        }
    }
    
    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;
    toast.innerHTML = `
        <span class="toast-icon">${getToastIcon(type)}</span>
        <span class="toast-message">${message}</span>
        <button class="toast-close">&times;</button>
    `;
    
    container.appendChild(toast);
    
    // Adjust position on mobile
    if (state.isMobile) {
        container.style.bottom = '80px';
        container.style.right = '10px';
        container.style.left = '10px';
    }
    
    // Trigger animation
    setTimeout(() => toast.classList.add('show'), 10);
    
    // Close button
    toast.querySelector('.toast-close').addEventListener('click', () => removeToast(toast));
    
    // Auto remove
    const autoRemoveTimeout = setTimeout(() => removeToast(toast), duration);
    
    // Add swipe to dismiss on mobile
    if (state.isMobile) {
        let startX = 0;
        
        toast.addEventListener('touchstart', (e) => {
            startX = e.touches[0].clientX;
        }, { passive: true });
        
        toast.addEventListener('touchend', (e) => {
            const endX = e.changedTouches[0].clientX;
            const deltaX = endX - startX;
            
            if (Math.abs(deltaX) > 50) {
                clearTimeout(autoRemoveTimeout);
                toast.style.transform = `translateX(${deltaX > 0 ? '400px' : '-400px'})`;
                setTimeout(() => removeToast(toast), 200);
            }
        }, { passive: true });
    }
}

function removeToast(toast) {
    toast.classList.remove('show');
    setTimeout(() => toast.remove(), 300);
}

function getToastIcon(type) {
    const icons = {
        success: '✓',
        error: '✗',
        warning: '⚠',
        info: 'ℹ'
    };
    return icons[type] || icons.info;
}


// ===== Advanced Features Implementation =====


// Export/Import Settings
function initializeExportImport() {
            const exportBtn = $('#export-settings');
            const importBtn = $('#import-settings');
            const importFile = $('#import-file');
            
            if (exportBtn) {
                exportBtn.addEventListener('click', exportSettings);
            }
            
            if (importBtn && importFile) {
                importBtn.addEventListener('click', () => importFile.click());
                importFile.addEventListener('change', handleImportFile);
            }
        }
        
        function exportSettings() {
            showLoading('Preparing export...');
            
            queueRequest('/api/settings')
                .then(response => response.json())
                .then(settings => {
                    const exportData = {
                        version: '1.0',
                        timestamp: new Date().toISOString(),
                        deviceName: '{{DEVICE_NAME}}',
                        settings: settings
                    };
                    
                    const blob = new Blob([JSON.stringify(exportData, null, 2)], { type: 'application/json' });
                    const url = URL.createObjectURL(blob);
                    const a = document.createElement('a');
                    a.href = url;
                    a.download = `esp32-settings-${new Date().toISOString().split('T')[0]}.json`;
                    document.body.appendChild(a);
                    a.click();
                    document.body.removeChild(a);
                    URL.revokeObjectURL(url);
                    
                    showToast('Settings exported successfully', 'success');
                })
                .catch(error => {
                    showToast('Failed to export settings: ' + error.message, 'error');
                })
                .finally(() => {
                    hideLoading();
                });
        }
        
        function handleImportFile(event) {
            const file = event.target.files[0];
            if (!file) return;
            
            if (!file.name.endsWith('.json')) {
                showToast('Please select a valid JSON file', 'error');
                return;
            }
            
            const reader = new FileReader();
            reader.onload = (e) => {
                try {
                    const importData = JSON.parse(e.target.result);
                    validateAndImportSettings(importData);
                } catch (error) {
                    showToast('Invalid settings file: ' + error.message, 'error');
                }
            };
            reader.readAsText(file);
            
            // Reset file input
            event.target.value = '';
        }
        
        function validateAndImportSettings(importData) {
            // Validate structure
            if (!importData.version || !importData.settings) {
                showToast('Invalid settings file format', 'error');
                return;
            }
            
            showConfirmDialog('Import settings? Current settings will be backed up first.', () => {
                // Backup current settings
                backupCurrentSettings().then(() => {
                    // Apply imported settings
                    applyImportedSettings(importData.settings);
                });
            });
        }
        
        function backupCurrentSettings() {
            return queueRequest('/api/settings')
                .then(response => response.json())
                .then(settings => {
                    localStorage.setItem('settingsBackup', JSON.stringify({
                        timestamp: new Date().toISOString(),
                        settings: settings
                    }));
                    showToast('Current settings backed up', 'info');
                });
        }
        
        function applyImportedSettings(settings) {
            showLoading('Importing settings...');
            
            // Save imported settings
            queueRequest('/api/settings', 'POST', JSON.stringify(settings), {
                'Content-Type': 'application/json'
            })
                .then(response => response.json())
                .then(() => {
                    showToast('Settings imported successfully', 'success');
                    setTimeout(() => {
                        window.location.reload();
                    }, 2000);
                })
                .catch(error => {
                    showToast('Failed to apply imported settings: ' + error.message, 'error');
                    // Offer to restore backup
                    showConfirmDialog('Restore previous settings from backup?', () => {
                        restoreBackup();
                    });
                })
                .finally(() => {
                    hideLoading();
                });
        }
        
        function restoreBackup() {
            const backup = localStorage.getItem('settingsBackup');
            if (!backup) {
                showToast('No backup found', 'error');
                return;
            }
            
            try {
                const backupData = JSON.parse(backup);
                applyImportedSettings(backupData.settings);
            } catch (error) {
                showToast('Failed to restore backup: ' + error.message, 'error');
            }
        }
        
        // Help System
        function initializeHelpTooltips() {
            const helpIcons = $$('.help-icon');
            const tooltip = $('#help-tooltip');
            
            if (!tooltip) return;
            
            helpIcons.forEach(icon => {
                icon.addEventListener('mouseenter', (e) => {
                    showHelpTooltip(e.target);
                });
                
                icon.addEventListener('mouseleave', () => {
                    hideHelpTooltip();
                });
                
                // Mobile support
                icon.addEventListener('click', (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    if (state.isMobile) {
                        showHelpTooltip(e.target);
                        setTimeout(hideHelpTooltip, 3000);
                    }
                });
            });
        }
        
        function showHelpTooltip(element) {
            const tooltip = $('#help-tooltip');
            const helpText = element.dataset.help;
            
            if (!tooltip || !helpText) return;
            
            tooltip.textContent = helpText;
            
            const rect = element.getBoundingClientRect();
            tooltip.style.left = rect.left + rect.width / 2 + 'px';
            tooltip.style.top = rect.bottom + 10 + 'px';
            
            tooltip.classList.add('visible');
        }
        
        function hideHelpTooltip() {
            const tooltip = $('#help-tooltip');
            if (tooltip) {
                tooltip.classList.remove('visible');
            }
        }
        
        // Keyboard Shortcuts
        function initializeKeyboardShortcuts() {
            const dialog = $('#keyboard-shortcuts-dialog');
            const closeBtn = dialog?.querySelector('.close-dialog');
            
            if (closeBtn) {
                closeBtn.addEventListener('click', () => {
                    dialog.classList.remove('visible');
                });
            }
            
            document.addEventListener('keydown', (e) => {
                // Ignore if typing in input
                if (e.target.matches('input, textarea, select')) {
                    // Except for Escape key
                    if (e.key === 'Escape') {
                        e.target.blur();
                        clearSearchIfActive();
                    }
                    return;
                }
                
                // Ctrl+S: Save settings
                if (e.ctrlKey && e.key === 's') {
                    e.preventDefault();
                    const activeForm = $('.tab-content.active form');
                    if (activeForm) {
                        saveSettings({ preventDefault: () => {} });
                    }
                }
                
                // Ctrl+F: Focus search
                if (e.ctrlKey && e.key === 'f') {
                    e.preventDefault();
                    const searchBox = $('#settings-search');
                    if (searchBox && state.activeTab === 'advanced-tab') {
                        searchBox.focus();
                        searchBox.select();
                    }
                }
                
                // Ctrl+E: Export settings
                if (e.ctrlKey && e.key === 'e') {
                    e.preventDefault();
                    exportSettings();
                }
                
                // ?: Show shortcuts help
                if (e.key === '?' && !e.ctrlKey && !e.altKey) {
                    e.preventDefault();
                    if (dialog) {
                        dialog.classList.toggle('visible');
                    }
                }
                
                // Esc: Close dialogs/clear search
                if (e.key === 'Escape') {
                    if (dialog && dialog.classList.contains('visible')) {
                        dialog.classList.remove('visible');
                    }
                    clearSearchIfActive();
                }
            });
        }
        
        function clearSearchIfActive() {
            const searchBox = $('#settings-search');
            if (searchBox && searchBox.value) {
                searchBox.value = '';
                clearSearchHighlights();
                $('#clear-search').classList.remove('visible');
            }
        }
        


// ===== Request Queue System =====
function queueRequest(url, method = 'GET', body = null, headers = null) {
    return new Promise((resolve, reject) => {
        state.requestQueue.push({ url, method, body, headers, resolve, reject, retryCount: 0 });
        processQueue();
    });
}

async function processQueue() {
    if (state.isProcessingQueue || state.requestQueue.length === 0) return;
    
    state.isProcessingQueue = true;
    const request = state.requestQueue.shift();
    
    try {
        const response = await fetchWithRetry(request);
        request.resolve(response);
    } catch (error) {
        request.reject(error);
    } finally {
        state.isProcessingQueue = false;
        if (state.requestQueue.length > 0) {
            setTimeout(processQueue, 100);
        }
    }
}

async function fetchWithRetry(request) {
    const { url, method, body, headers, retryCount } = request;
    
    try {
        const options = { method };
        
        if (headers) {
            options.headers = headers;
        } else if (body) {
            options.headers = { 'Content-Type': 'application/x-www-form-urlencoded' };
        }
        
        if (body) {
            options.body = body;
        }
        
        const response = await fetch(url, options);
        
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}: ${response.statusText}`);
        }
        
        return response;
    } catch (error) {
        if (retryCount < RETRY_DELAYS.length) {
            const delay = RETRY_DELAYS[retryCount];
            console.log(`Retrying request to ${url} in ${delay}ms (attempt ${retryCount + 1})`);
            
            await new Promise(resolve => setTimeout(resolve, delay));
            request.retryCount++;
            return fetchWithRetry(request);
        }
        
        throw error;
    }
}

// ===== Confirmation Dialog =====
function showConfirmDialog(message, onConfirm, onCancel = null) {
    const overlay = document.createElement('div');
    overlay.className = 'confirm-overlay';
    
    const dialog = document.createElement('div');
    dialog.className = 'confirm-dialog';
    dialog.innerHTML = `
        <div class="confirm-message">${message}</div>
        <div class="confirm-buttons">
            <button class="confirm-yes">Confirm</button>
            <button class="secondary confirm-no">Cancel</button>
        </div>
    `;
    
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);
    
    // Show animation
    setTimeout(() => overlay.classList.add('show'), 10);
    
    const cleanup = () => {
        overlay.classList.remove('show');
        setTimeout(() => overlay.remove(), 300);
    };
    
    dialog.querySelector('.confirm-yes').addEventListener('click', () => {
        cleanup();
        if (onConfirm) onConfirm();
    });
    
    dialog.querySelector('.confirm-no').addEventListener('click', () => {
        cleanup();
        if (onCancel) onCancel();
    });
}

// ===== Utility Functions =====
function debounce(func, wait) {
    let timeout;
    return function executedFunction(...args) {
        const later = () => {
            clearTimeout(timeout);
            func(...args);
        };
        clearTimeout(timeout);
        timeout = setTimeout(later, wait);
    };
}

// ===== Legacy Functions (Updated) =====
window.openTab = function(evt, tabName) {
    switchTab(tabName);
};

function updateStatusStyles(status) {
    const statusEl = $('#connection-status');
    if (!statusEl) return;
    
    // Remove all status classes
    statusEl.classList.remove('status-connected', 'status-connecting', 'status-failed', 'status-ap', 'status-ap-only');
    
    // Add appropriate class based on status
    if (status === 'Connected') {
        statusEl.classList.add('status-connected');
    } else if (status === 'Connecting...') {
        statusEl.classList.add('status-connecting');
    } else if (status === 'Connection failed') {
        statusEl.classList.add('status-failed');
    } else if (status === 'Access Point Mode') {
        statusEl.classList.add('status-ap');
    } else if (status === 'AP-Only Mode') {
        statusEl.classList.add('status-ap-only');
        // Hide SSID row in AP-Only mode
        const ssidRow = document.getElementById('current-ssid-row');
        if (ssidRow) {
            ssidRow.style.display = 'none';
        }
    }
}

function loadSettings() {
    showLoading('Loading settings...');
    
    queueRequest('/api/settings')
        .then(response => response.json())
        .then(settings => {
            // Store original settings for undo
            state.originalSettings = { ...settings };
            
            // Map of field names to their parent form IDs
            const fieldFormMap = {
                // Mode settings (receiver)
                'sample_rate': 'mode-settings-form',
                'bit_depth': 'mode-settings-form',
                'volume': 'mode-settings-form',
                'use_direct_write': 'mode-settings-form',
                'initial_buffer_size': 'mode-settings-form',
                'buffer_grow_step_size': 'mode-settings-form',
                'max_buffer_size': 'mode-settings-form',
                'max_grow_size': 'mode-settings-form',
                'spdif_data_pin': 'mode-settings-form',
                
                // Mode settings (sender)
                'sender_destination_ip': 'mode-settings-form',
                'sender_destination_port': 'mode-settings-form',
                'enable_usb_sender': 'mode-settings-form',
                'enable_spdif_sender': 'mode-settings-form',
                
                // AP Host settings
                'port': 'aphost-settings-form',
                'ap_ssid': 'aphost-settings-form',
                'ap_password': 'aphost-settings-form',
                'hide_ap_when_connected': 'aphost-settings-form',
                'rssi_threshold': 'aphost-settings-form',
                
                // Advanced settings
                'silence_threshold_ms': 'advanced-settings-form',
                'silence_amplitude_threshold': 'advanced-settings-form',
                'network_check_interval_ms': 'advanced-settings-form',
                'activity_threshold_packets': 'advanced-settings-form',
                'network_inactivity_timeout_ms': 'advanced-settings-form'
            };
            
            // Apply settings to form fields
            Object.keys(settings).forEach(key => {
                let element = null;
                
                // First try to find element within its specific form
                if (fieldFormMap[key]) {
                    const form = document.getElementById(fieldFormMap[key]);
                    if (form) {
                        element = form.querySelector(`#${key}`);
                    }
                }
                
                // Fallback to global search if not in form map or not found
                if (!element) {
                    element = document.getElementById(key);
                }
                
                if (element) {
                    if (element.type === 'checkbox') {
                        element.checked = settings[key] === true || settings[key] === 'true' || settings[key] === 1;
                    } else {
                        element.value = settings[key];
                    }
                }
            });
            
            // Update sender fields visibility
            updateSenderOptionsVisibility();
            
            showSettingsSuccess('Settings loaded successfully');
        })
        .catch(error => {
            showSettingsAlert('Failed to load settings: ' + error.message);
        })
        .finally(() => {
            hideLoading();
        });
}

function saveSettings(event) {
    if (event) event.preventDefault();
    
    // Collect all settings from all forms
    const settings = {};
    
    // Get all settings forms
    const forms = ['mode-settings-form', 'aphost-settings-form', 'advanced-settings-form'];
    
    forms.forEach(formId => {
        const form = document.getElementById(formId);
        if (!form) return;
        
        // Get all inputs in the form
        const formData = new FormData(form);
        
        // Convert form data to JSON object
        for (let [key, value] of formData.entries()) {
            if (!isNaN(value) && key !== 'ap_password') {
                if (key === 'volume') {
                    settings[key] = parseFloat(value);
                } else {
                    settings[key] = parseInt(value, 10);
                }
            } else {
                settings[key] = value;
            }
        }
        
        // Handle checkbox values
        form.querySelectorAll('input[type="checkbox"]').forEach(checkbox => {
            settings[checkbox.id] = checkbox.checked;
        });
    });
    
    // Find the active form for button loading
    const activeTab = $('.tab-content.active');
    const activeForm = activeTab ? activeTab.querySelector('form') : null;
    
    if (activeForm && !validateForm(activeForm)) {
        showToast('  validation errors', 'error');
        scrollToFirstError();
        return;
    }
    
    const button = activeForm ? activeForm.querySelector('button[type="submit"]') : null;
    if (button) setButtonLoading(button, true);
    
    showLoading('Saving settings...');
    
    queueRequest('/api/settings', 'POST', JSON.stringify(settings), {
        'Content-Type': 'application/json'
    })
        .then(response => response.json())
        .then(result => {
            showSettingsSuccess('Settings saved successfully');
            if (activeForm) {
                const tabContent = activeForm.closest('.tab-content');
                clearUnsavedMarks(tabContent);
            }
            
            // Update original settings
            Object.assign(state.originalSettings, settings);
        })
        .catch(error => {
            showSettingsAlert('Failed to save settings: ' + error.message);
        })
        .finally(() => {
            if (button) setButtonLoading(button, false);
            hideLoading();
        });
}

function saveConfiguration(event) {
    if (event) event.preventDefault();
    
    const form = $('#wifi-form');
    if (!validateForm(form)) {
        showToast('Please fix validation errors', 'error');
        scrollToFirstError();
        return;
    }
    
    const ssid = $('#ssid').value.trim();
    const password = $('#password').value;
    
    if (!ssid) {
        showAlert('Please enter or select a WiFi network');
        return;
    }
    
    const button = form.querySelector('button[type="submit"]');
    setButtonLoading(button, true);
    
    const formData = new URLSearchParams();
    formData.append('ssid', ssid);
    formData.append('password', password);
    
    $('#alert-message').style.display = 'none';
    $('#success-message').style.display = 'none';
    
    queueRequest('/connect', 'POST', formData.toString())
        .then(response => response.text())
        .then(result => {
            showSuccess('Connecting to WiFi network...');
            $('#connection-status').textContent = 'Connecting...';
            updateStatusStyles('Connecting...');
            
            // Show status check container after a delay
            setTimeout(() => {
                $('#status-check-container').style.display = 'block';
            }, 5000);
        })
        .catch(error => {
            showAlert('Failed to connect: ' + error.message);
        })
        .finally(() => {
            setButtonLoading(button, false);
        });
}

function forgetNetwork() {
    showConfirmDialog('Are you sure you want to reset all WiFi settings? This will reboot the device into Access Point mode.', () => {
        showLoading('Resetting WiFi settings...');
        
        queueRequest('/reset', 'POST')
            .then(response => response.text())
            .then(result => {
                showSuccess('WiFi settings reset. Device will reboot into Access Point mode shortly.');
            })
            .catch(error => {
                showAlert('Failed to reset WiFi settings: ' + error.message);
            })
            .finally(() => {
                hideLoading();
            });
    });
}

function startPairingMode() {
    showConfirmDialog('Start pairing mode? The device will search for unconfigured ESP32 Screan devices and configure them with current WiFi settings.', () => {
        // Show loading state
        showToast('Starting pairing mode...', 'info');
        
        queueRequest('/api/start_pairing', 'POST', null, {
            'Content-Type': 'application/json'
        })
        .then(response => response.json())
        .then(data => {
            if (data.status === 'success') {
                showToast('Pairing mode started. Searching for devices...', 'success');
                // Show status for 30 seconds (pairing timeout)
                setTimeout(() => {
                    showToast('Pairing mode completed', 'info');
                }, 30000);
            } else {
                showToast('Failed to start pairing mode: ' + (data.message || 'Unknown error'), 'error');
            }
        })
        .catch(error => {
            console.error('Error:', error);
            showToast('Error occurred while starting pairing mode: ' + error.message, 'error');
        });
    });

// Expose function to window for HTML onclick handler

function cancelPairingMode() {
    showToast('Cancelling pairing mode...', 'info');
    
    queueRequest('/api/cancel_pairing', 'POST', null, {
        'Content-Type': 'application/json'
    })
    .then(response => response.json())
    .then(data => {
        if (data.status === 'success') {
            showToast('Pairing mode cancelled', 'success');
            // Show pair button, hide cancel button
            const pairBtn = document.getElementById('pair-device-btn');
            const cancelBtn = document.getElementById('cancel-pairing-btn');
            if (pairBtn) pairBtn.style.display = '';
            if (cancelBtn) cancelBtn.style.display = 'none';
        } else {
            showToast('Failed to cancel pairing: ' + (data.message || 'Unknown error'), 'error');
        }
    })
    .catch(error => {
        console.error('Error:', error);
        showToast('Error cancelling pairing mode: ' + error.message, 'error');
    });
}

// Expose to window for HTML onclick handler
window.cancelPairingMode = cancelPairingMode;

// Update the existing startPairingMode to handle button visibility
const originalStartPairingMode = startPairingMode;
window.startPairingMode = function() {
    showConfirmDialog('Start pairing mode? The device will search for unconfigured ESP32 devices and configure them with current WiFi settings.', () => {
        // Show loading state
        showToast('Starting pairing mode...', 'info');
        
        queueRequest('/api/start_pairing', 'POST', null, {
            'Content-Type': 'application/json'
        })
        .then(response => response.json())
        .then(data => {
            if (data.status === 'success') {
                showToast('Pairing mode started. Searching for devices...', 'success');
                
                // Hide pair button, show cancel button
                const pairBtn = document.getElementById('pair-device-btn');
                const cancelBtn = document.getElementById('cancel-pairing-btn');
                if (pairBtn) pairBtn.style.display = 'none';
                if (cancelBtn) cancelBtn.style.display = '';
                
                // Auto-cancel after 30 seconds
                setTimeout(() => {
                    const cancelBtn = document.getElementById('cancel-pairing-btn');
                    if (cancelBtn && cancelBtn.style.display !== 'none') {
                        showToast('Pairing mode timed out', 'info');
                        cancelPairingMode();
                    }
                }, 30000);
            } else {
                showToast('Failed to start pairing mode: ' + (data.message || 'Unknown error'), 'error');
            }
        })
        .catch(error => {
            console.error('Error:', error);
            showToast('Error occurred while starting pairing mode: ' + error.message, 'error');
        });
    });
};
window.startPairingMode = startPairingMode;
}

function checkConnectionStatus() {
    const button = $('#check-status-button');
    setButtonLoading(button, true);
    
    queueRequest('/status')
        .then(response => response.json())
        .then(status => {
            $('#connection-status').textContent = status.status;
            updateStatusStyles(status.status);
            
            if (status.connected) {
                showSuccess('Connected successfully! Redirecting...');
                setTimeout(() => {
                    window.location.reload();
                }, 2000);
            } else if (status.status === 'Connection failed') {
                showAlert('Failed to connect to the network. Please check your credentials and try again.');
                $('#status-check-container').style.display = 'none';
            }
        })
        .catch(error => {
            showAlert('Failed to check status: ' + error.message);
        })
        .finally(() => {
            setButtonLoading(button, false);
        });
}

// Legacy alert/success functions mapped to toast
function showSettingsAlert(message) {
    const alerts = $$('#settings-alert');
    alerts.forEach(alert => {
        alert.textContent = message;
        alert.style.display = 'block';
        alert.classList.remove('hidden');
    });
    
    $$('#settings-success').forEach(el => el.style.display = 'none');
    
    setTimeout(() => {
        alerts.forEach(alert => alert.style.display = 'none');
    }, 5000);
    
    showToast(message, 'error');
}

function showSettingsSuccess(message) {
    const successes = $$('#settings-success');
    successes.forEach(success => {
        success.textContent = message;
        success.style.display = 'block';
        success.classList.remove('hidden');
    });
    
    $$('#settings-alert').forEach(el => el.style.display = 'none');
    
    setTimeout(() => {
        successes.forEach(success => success.style.display = 'none');
    }, 5000);
    
    showToast(message, 'success');
}

// CSS for new features is now in index.html, no injection needed

// ===== Performance Monitoring =====
if (state.isMobile && 'connection' in navigator) {
    navigator.connection.addEventListener('change', () => {
        const connection = navigator.connection;
        if (connection.saveData || connection.effectiveType === '2g') {
            document.body.classList.add('save-data');
        } else {
            document.body.classList.remove('save-data');
        }
    });
}

// ===== Page Visibility =====
document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
        // Pause any ongoing operations if needed
    } else {
        // Resume operations
        if (state.activeTab === 'connection-tab') {
            // Could refresh network list
        }
    }
});

// ===== Setup Wizard Implementation =====
// Wizard moved to separate files (wizard.html, wizard.js, wizard.css)
// Now loaded via iframe for better modularity

// Initialize wizard on page load
document.addEventListener('DOMContentLoaded', function() {
    // Check if wizard should be shown
    checkAndInitializeWizard();
});

function checkAndInitializeWizard() {
    // Query settings to check if wizard is needed
    queueRequest('/api/settings')
        .then(response => response.json())
        .then(settings => {
            // Check if wizard should be shown (setup_wizard_completed is false/undefined)
            if (!settings.setup_wizard_completed) {
                showSetupWizard();
            }
        })
        .catch(error => {
            console.error('Failed to check wizard status:', error);
        });
}

function showSetupWizard() {
    // Use existing wizard overlay from HTML
    const wizardOverlay = document.getElementById('setup-wizard-overlay');
    if (!wizardOverlay) {
        console.warn('Setup wizard overlay not found in HTML');
        return;
    }
    
    // Show the wizard
    wizardOverlay.style.display = 'flex';
    wizardOverlay.classList.add('active');
    
    // Load the wizard HTML
    const iframe = wizardOverlay.querySelector('iframe');
    if (iframe) {
        iframe.src = '/wizard.html';
    }
}

function closeWizard() {
    const overlay = document.getElementById('setup-wizard-overlay');
    if (overlay) {
        overlay.classList.remove('active');
        setTimeout(() => {
            overlay.style.display = 'none';
            // Clear iframe src to reset wizard state
            const iframe = overlay.querySelector('iframe');
            if (iframe) {
                iframe.src = '';
            }
        }, 300);
    }
}

// Handle messages from wizard iframe
window.addEventListener('message', function(event) {
    // Verify origin is from same domain
    if (event.origin !== window.location.origin) {
        return;
    }
    
    const data = event.data;
    
    switch(data.type) {
        case 'wizard-close':
        case 'wizard-closed':
            closeWizard();
            break;
            
        case 'wizard-complete':
            closeWizard();
            // Reload page if settings changed
            if (data.reload) {
                setTimeout(() => {
                    window.location.reload();
                }, data.delay || 3000);
            }
            break;
        
        case 'wizard-redirect':
            // Handle redirect after wizard completion
            window.location.reload();
            break;
            
        case 'api-request':
            // Proxy API request from wizard
            handleWizardRequest(data, event.source);
            break;
    }
});

// Handle API requests from wizard iframe
function handleWizardRequest(request, source) {
    const { requestId, url, method, body, headers } = request;
    
    // Use the existing queueRequest function
    queueRequest(url, method, body, headers)
        .then(response => {
            // Convert response to JSON if possible
            const contentType = response.headers?.get ? response.headers.get('content-type') : null;
            if (contentType && contentType.includes('application/json')) {
                return response.json();
            }
            return response.text();
        })
        .then(data => {
            // Send success response back to wizard
            source.postMessage({
                type: 'api-response',
                requestId: requestId,
                response: data
            }, window.location.origin);
        })
        .catch(error => {
            // Send error response back to wizard
            source.postMessage({
                type: 'api-response',
                requestId: requestId,
                error: error.message || error.toString()
            }, window.location.origin);
        });
}

// Make wizard functions available globally
window.showSetupWizard = showSetupWizard;
window.closeWizard = closeWizard;

// ===== OTA Update Functions =====
const OTA = {
    maxFileSize: 4 * 1024 * 1024, // 4MB max
    currentFile: null,
    uploadXHR: null,
    isUploading: false,
    reconnectTimer: null,
    statusCheckTimer: null,
    uploadStartTime: 0,
    lastProgressTime: 0,
    lastProgressBytes: 0
};

// Initialize OTA functionality
function initializeOTA() {
    const fileInput = $('#ota-file-input');
    const dropzone = $('#upload-dropzone');
    const uploadArea = $('#ota-upload-area');
    
    if (!fileInput || !dropzone) return;
    
    // File input change handler
    fileInput.addEventListener('change', handleOTAFileSelect);
    
    // Drag and drop handlers
    dropzone.addEventListener('dragover', handleOTADragOver);
    dropzone.addEventListener('dragleave', handleOTADragLeave);
    dropzone.addEventListener('drop', handleOTADrop);
    
    // Prevent default drag behaviors on the whole upload area
    uploadArea.addEventListener('dragover', (e) => e.preventDefault());
    uploadArea.addEventListener('drop', (e) => e.preventDefault());
    
    // Load current version info
    loadVersionInfo();
}

// Load and display current version information
function loadVersionInfo() {
    queueRequest('/api/ota/version')
        .then(response => response.json())
        .then(data => {
            $('#current-version').textContent = data.version || '1.0.0';
            $('#build-date').textContent = data.build_date || 'Unknown';
            $('#platform').textContent = data.platform || 'ESP32';
            
            // Free space display is not available via API
            // Could be added to /api/ota/version response in future
            $('#free-space').textContent = 'N/A';
        })
        .catch(error => {
            console.error('Failed to load version info:', error);
            $('#current-version').textContent = 'Unknown';
            $('#build-date').textContent = 'Unknown';
            $('#platform').textContent = 'Unknown';
        });
}

// Handle OTA file selection
function handleOTAFileSelect(event) {
    const file = event.target.files[0];
    if (file) {
        selectOTAFile(file);
    }
}

// Handle drag over
function handleOTADragOver(event) {
    event.preventDefault();
    event.stopPropagation();
    event.dataTransfer.dropEffect = 'copy';
    $('#upload-dropzone').classList.add('dragover');
}

// Handle drag leave
function handleOTADragLeave(event) {
    event.preventDefault();
    event.stopPropagation();
    $('#upload-dropzone').classList.remove('dragover');
}

// Handle file drop
function handleOTADrop(event) {
    event.preventDefault();
    event.stopPropagation();
    $('#upload-dropzone').classList.remove('dragover');
    
    const files = event.dataTransfer.files;
    if (files.length > 0) {
        selectOTAFile(files[0]);
    }
}

// Select and validate OTA file
function selectOTAFile(file) {
    // Reset any previous state
    resetOTAInterface();
    
    // Basic validation
    if (!file.name.endsWith('.bin')) {
        showOTAError('Invalid file type. Please select a .bin firmware file.');
        return;
    }
    
    if (file.size > OTA.maxFileSize) {
        showOTAError(`File too large. Maximum size is ${OTA.maxFileSize / (1024*1024)}MB.`);
        return;
    }
    
    if (file.size < 1024) { // Less than 1KB is suspicious
        showOTAError('File too small. This doesn\'t appear to be a valid firmware file.');
        return;
    }
    
    // Store file reference
    OTA.currentFile = file;
    
    // Update UI
    $('#upload-dropzone').style.display = 'none';
    $('#file-selected').style.display = 'block';
    $('#file-name').textContent = file.name;
    $('#file-size').textContent = formatFileSize(file.size);
    
    // Validate file
    validateOTAFile(file);
}

// Validate OTA firmware file
function validateOTAFile(file) {
    const sizeCheck = $('#size-check');
    const typeCheck = $('#type-check');
    
    let isValid = true;
    
    // Check file size
    if (file.size > 0 && file.size <= OTA.maxFileSize) {
        sizeCheck.querySelector('.check-icon').textContent = '✅';
        sizeCheck.querySelector('.check-text').textContent = `File size OK (${formatFileSize(file.size)})`;
        sizeCheck.classList.add('valid');
    } else {
        sizeCheck.querySelector('.check-icon').textContent = '❌';
        sizeCheck.querySelector('.check-text').textContent = 'File size invalid';
        sizeCheck.classList.add('invalid');
        isValid = false;
    }
    
    // Check file type (basic check for .bin extension and magic bytes)
    const reader = new FileReader();
    reader.onload = function(e) {
        const buffer = e.target.result;
        const view = new Uint8Array(buffer);
        
        // Check for ESP32 firmware magic byte (0xE9)
        if (view.length > 0 && view[0] === 0xE9) {
            typeCheck.querySelector('.check-icon').textContent = '✅';
            typeCheck.querySelector('.check-text').textContent = 'Valid ESP32 firmware detected';
            typeCheck.classList.add('valid');
        } else if (view.length > 0) {
            // Allow upload anyway but warn
            typeCheck.querySelector('.check-icon').textContent = '⚠️';
            typeCheck.querySelector('.check-text').textContent = 'File may not be ESP32 firmware';
            typeCheck.classList.add('warning');
        } else {
            typeCheck.querySelector('.check-icon').textContent = '❌';
            typeCheck.querySelector('.check-text').textContent = 'Invalid file';
            typeCheck.classList.add('invalid');
            isValid = false;
        }
        
        // Enable/disable upload button
        $('#ota-upload-btn').disabled = !isValid;
    };
    
    // Read first 256 bytes for validation
    reader.readAsArrayBuffer(file.slice(0, 256));
}

// Remove selected OTA file
function removeOTAFile() {
    OTA.currentFile = null;
    $('#file-selected').style.display = 'none';
    $('#upload-dropzone').style.display = 'flex';
    $('#ota-upload-btn').disabled = true;
    $('#ota-file-input').value = '';
}

// Start OTA update process
function startOTAUpdate() {
    if (!OTA.currentFile) {
        showOTAError('No firmware file selected');
        return;
    }
    
    if (OTA.isUploading) {
        showOTAError('Update already in progress');
        return;
    }
    
    // Confirm update
    showConfirmDialog(
        'Start firmware update? The device will restart after successful update.',
        () => performOTAUpdate(),
        null
    );
}

// Perform the actual OTA update
function performOTAUpdate() {
    OTA.isUploading = true;
    OTA.uploadStartTime = Date.now();
    OTA.lastProgressTime = Date.now();
    OTA.lastProgressBytes = 0;
    
    // Update UI
    $('#file-selected').style.display = 'none';
    $('#ota-progress-section').style.display = 'block';
    $('#ota-upload-btn').style.display = 'none';
    $('#ota-cancel-btn').style.display = 'inline-block';
    $('#progress-status .status-text').textContent = 'Uploading firmware...';
    
    // Create XHR for progress tracking
    OTA.uploadXHR = new XMLHttpRequest();
    
    // Progress handler
    OTA.uploadXHR.upload.addEventListener('progress', handleOTAProgress);
    
    // Load handler
    OTA.uploadXHR.addEventListener('load', handleOTAComplete);
    
    // Error handler
    OTA.uploadXHR.addEventListener('error', handleOTAError);
    
    // Abort handler
    OTA.uploadXHR.addEventListener('abort', handleOTAAbort);
    
    // Open connection and send
    OTA.uploadXHR.open('POST', '/api/ota/upload');
    
    // Set content type for binary data
    OTA.uploadXHR.setRequestHeader('Content-Type', 'application/octet-stream');
    OTA.uploadXHR.setRequestHeader('Content-Length', OTA.currentFile.size.toString());
    
    // Send the raw binary file directly
    OTA.uploadXHR.send(OTA.currentFile);
    
    // Start status polling
    startOTAStatusPolling();
}

// Handle OTA upload progress
function handleOTAProgress(event) {
    if (!event.lengthComputable) return;
    
    const percentComplete = Math.round((event.loaded / event.total) * 100);
    const currentTime = Date.now();
    const elapsedTime = (currentTime - OTA.uploadStartTime) / 1000;
    const timeDiff = (currentTime - OTA.lastProgressTime) / 1000;
    const bytesDiff = event.loaded - OTA.lastProgressBytes;
    
    // Update progress bar
    $('#progress-percentage').textContent = `${percentComplete}%`;
    $('#progress-bar-fill').style.width = `${percentComplete}%`;
    
    // Update bytes transferred
    $('#bytes-transferred').textContent =
        `${formatFileSize(event.loaded)} / ${formatFileSize(event.total)}`;
    
    // Calculate and update speed
    if (timeDiff > 0) {
        const speed = bytesDiff / timeDiff;
        $('#upload-speed').textContent = `${formatFileSize(speed)}/s`;
        
        // Estimate time remaining
        if (speed > 0) {
            const remainingBytes = event.total - event.loaded;
            const remainingSeconds = Math.round(remainingBytes / speed);
            $('#time-remaining').textContent = formatTime(remainingSeconds);
        }
    }
    
    // Update last progress tracking
    OTA.lastProgressTime = currentTime;
    OTA.lastProgressBytes = event.loaded;
}

// Handle OTA upload completion
function handleOTAComplete(event) {
    const response = OTA.uploadXHR.response;
    let responseData;
    
    try {
        responseData = JSON.parse(response);
    } catch (e) {
        responseData = { status: 'error', message: 'Invalid response from server' };
    }
    
    if (OTA.uploadXHR.status === 200 && responseData.status === 'success') {
        // Update UI to show verification/installation phase
        $('#progress-status .status-icon').textContent = '🔄';
        $('#progress-status .status-text').textContent = 'Installing firmware...';
        $('#ota-cancel-btn').style.display = 'none';
        
        // Device will restart, start monitoring for reconnection
        setTimeout(() => {
            startReconnectMonitoring();
        }, 3000);
    } else {
        // Handle error
        const errorMsg = responseData.message || `Upload failed with status ${OTA.uploadXHR.status}`;
        handleOTAError(new Error(errorMsg));
    }
}

// Handle OTA upload error
function handleOTAError(error) {
    OTA.isUploading = false;
    stopOTAStatusPolling();
    
    const errorMessage = error.message || error.toString() || 'Unknown error occurred';
    
    // Update UI
    $('#ota-progress-section').style.display = 'none';
    $('#ota-result').style.display = 'block';
    $('#result-error').style.display = 'block';
    $('#error-message').textContent = errorMessage;
    
    // Log error
    console.error('OTA update error:', error);
    
    // Show alert
    showOTAError(`Update failed: ${errorMessage}`);
}

// Handle OTA upload abort
function handleOTAAbort() {
    OTA.isUploading = false;
    stopOTAStatusPolling();
    
    // Reset UI
    resetOTAInterface();
    showOTAAlert('Update cancelled');
}

// Cancel OTA update
function cancelOTAUpdate() {
    if (OTA.uploadXHR && OTA.isUploading) {
        showConfirmDialog(
            'Cancel firmware update? This will abort the upload process.',
            () => {
                OTA.uploadXHR.abort();
                OTA.isUploading = false;
                stopOTAStatusPolling();
                resetOTAInterface();
            },
            null
        );
    }
}

// Start polling OTA status
function startOTAStatusPolling() {
    // Poll every 2 seconds
    OTA.statusCheckTimer = setInterval(() => {
        if (!OTA.isUploading) {
            stopOTAStatusPolling();
            return;
        }
        
        // Check OTA status
        fetch('/api/ota/status')
            .then(response => response.json())
            .then(data => {
                if (data.status === 'updating') {
                    // Update in progress
                    if (data.progress) {
                        $('#progress-status .status-text').textContent = data.message || 'Updating...';
                    }
                } else if (data.status === 'success') {
                    // Update successful, device will restart
                    stopOTAStatusPolling();
                    startReconnectMonitoring();
                } else if (data.status === 'error') {
                    // Update failed
                    stopOTAStatusPolling();
                    handleOTAError(new Error(data.message || 'Update failed'));
                }
            })
            .catch(error => {
                // Connection lost, might be restarting
                console.log('Status check failed, device may be restarting');
            });
    }, 2000);
}

// Stop polling OTA status
function stopOTAStatusPolling() {
    if (OTA.statusCheckTimer) {
        clearInterval(OTA.statusCheckTimer);
        OTA.statusCheckTimer = null;
    }
}

// Start monitoring for device reconnection after update
function startReconnectMonitoring() {
    OTA.isUploading = false;
    
    // Update UI
    $('#ota-progress-section').style.display = 'none';
    $('#ota-result').style.display = 'block';
    $('#result-success').style.display = 'block';
    
    // Start countdown
    let countdown = 30;
    const countdownElement = $('#reconnect-countdown');
    
    OTA.reconnectTimer = setInterval(() => {
        countdown--;
        countdownElement.textContent = countdown;
        
        // Try to ping the device
        fetch('/api/ota/version', { method: 'HEAD' })
            .then(() => {
                // Device is back online
                clearInterval(OTA.reconnectTimer);
                showOTASuccess('Update completed successfully! Device is back online.');
                setTimeout(() => {
                    location.reload();
                }, 2000);
            })
            .catch(() => {
                // Still offline
                if (countdown <= 0) {
                    clearInterval(OTA.reconnectTimer);
                    location.reload();
                }
            });
    }, 1000);
}

// Reset OTA interface to initial state
function resetOTAInterface() {
    OTA.currentFile = null;
    OTA.isUploading = false;
    
    // Reset file input
    $('#ota-file-input').value = '';
    
    // Reset UI
    $('#file-selected').style.display = 'none';
    $('#upload-dropzone').style.display = 'flex';
    $('#ota-progress-section').style.display = 'none';
    $('#ota-result').style.display = 'none';
    $('#result-success').style.display = 'none';
    $('#result-error').style.display = 'none';
    $('#ota-upload-btn').style.display = 'inline-block';
    $('#ota-upload-btn').disabled = true;
    $('#ota-cancel-btn').style.display = 'none';
    
    // Reset progress
    $('#progress-percentage').textContent = '0%';
    $('#progress-bar-fill').style.width = '0%';
    
    // Clear timers
    stopOTAStatusPolling();
    if (OTA.reconnectTimer) {
        clearInterval(OTA.reconnectTimer);
        OTA.reconnectTimer = null;
    }
}

// Check for online updates
function checkForUpdates() {
    showOTAAlert('Checking for updates...');
    
    queueRequest('/api/check_updates')
        .then(response => response.json())
        .then(data => {
            if (data.update_available) {
                showConfirmDialog(
                    `New version ${data.latest_version} is available. Current version is ${data.current_version}. Download and install?`,
                    () => downloadAndInstallUpdate(data.download_url),
                    null
                );
            } else {
                showOTASuccess('Your device is running the latest firmware version.');
            }
        })
        .catch(error => {
            showOTAError('Failed to check for updates: ' + error.message);
        });
}

// Download and install update from URL
function downloadAndInstallUpdate(url) {
    showOTAAlert('Downloading update...');
    
    queueRequest('/api/download_update', 'POST', JSON.stringify({ url: url }), {
        'Content-Type': 'application/json'
    })
        .then(response => response.json())
        .then(data => {
            if (data.status === 'success') {
                showOTASuccess('Update download started. Device will restart when complete.');
                startReconnectMonitoring();
            } else {
                showOTAError('Failed to download update: ' + data.message);
            }
        })
        .catch(error => {
            showOTAError('Failed to start update download: ' + error.message);
        });
}

// Show OTA-specific alerts
function showOTAAlert(message) {
    const alert = $('#ota-alert');
    if (alert) {
        alert.textContent = message;
        alert.style.display = 'block';
        alert.classList.remove('hidden');
        $('#ota-success').style.display = 'none';
        
        setTimeout(() => {
            alert.style.display = 'none';
        }, 5000);
    }
    showToast(message, 'info');
}

function showOTASuccess(message) {
    const success = $('#ota-success');
    if (success) {
        success.textContent = message;
        success.style.display = 'block';
        success.classList.remove('hidden');
        $('#ota-alert').style.display = 'none';
        
        setTimeout(() => {
            success.style.display = 'none';
        }, 5000);
    }
    showToast(message, 'success');
}

function showOTAError(message) {
    const alert = $('#ota-alert');
    if (alert) {
        alert.textContent = message;
        alert.style.display = 'block';
        alert.classList.remove('hidden');
        $('#ota-success').style.display = 'none';
        
        setTimeout(() => {
            alert.style.display = 'none';
        }, 10000);
    }
    showToast(message, 'error');
}

// Utility functions for OTA
function formatFileSize(bytes) {
    if (bytes === 0) return '0 Bytes';
    const k = 1024;
    const sizes = ['Bytes', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return Math.round((bytes / Math.pow(k, i)) * 100) / 100 + ' ' + sizes[i];
}

function formatTime(seconds) {
    if (seconds < 60) {
        return `${seconds}s`;
    } else if (seconds < 3600) {
        const minutes = Math.floor(seconds / 60);
        const secs = seconds % 60;
        return `${minutes}m ${secs}s`;
    } else {
        const hours = Math.floor(seconds / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        return `${hours}h ${minutes}m`;
    }
}

// Initialize OTA on page load
document.addEventListener('DOMContentLoaded', function() {
    // Initialize OTA if on OTA tab
    const otaTab = $('#ota-tab');
    if (otaTab) {
        initializeOTA();
    }
});

// Make OTA functions available globally
window.startOTAUpdate = startOTAUpdate;
window.cancelOTAUpdate = cancelOTAUpdate;
window.removeOTAFile = removeOTAFile;
window.resetOTAInterface = resetOTAInterface;
window.checkForUpdates = checkForUpdates;

// ===== Logs Viewer Functions =====
const LogsViewer = {
    autoRefreshInterval: 3000, // Default 3 seconds
    autoRefreshTimer: null,
    autoScroll: true,
    autoRefresh: true,
    lineCount: 50,
    lastLogPosition: 0,
    isRefreshing: false
};

// Initialize logs viewer
function initializeLogsViewer() {
    // Set initial values from UI
    LogsViewer.lineCount = parseInt($('#log-line-count')?.value || 50);
    LogsViewer.autoRefresh = $('#log-auto-refresh')?.checked !== false;
    LogsViewer.autoScroll = $('#log-auto-scroll')?.checked !== false;
    LogsViewer.autoRefreshInterval = parseInt($('#log-refresh-interval')?.value || 3000);
    
    // Start auto-refresh if enabled
    if (LogsViewer.autoRefresh) {
        startLogsAutoRefresh();
    }
    
    // Initial load
    refreshLogs();
}

// Refresh logs from server
function refreshLogs() {
    if (LogsViewer.isRefreshing) {
        console.log('Already refreshing logs, skipping...');
        return;
    }
    
    LogsViewer.isRefreshing = true;
    const statusEl = $('#logs-status');
    const statusIndicator = statusEl?.querySelector('.status-indicator');
    const statusText = statusEl?.querySelector('.status-text');
    
    // Update status
    if (statusIndicator) statusIndicator.className = 'status-indicator loading';
    if (statusText) statusText.textContent = 'Loading...';
    
    // Build query parameters
    const params = new URLSearchParams({
        lines: LogsViewer.lineCount
    });
    
    queueRequest(`/api/logs?${params}`)
        .then(response => response.json())
        .then(data => {
            displayLogs(data);
            updateLogsStatus('success', 'Updated');
            
            // Update last updated timestamp
            const timestamp = new Date().toLocaleTimeString();
            $('#logs-last-updated').textContent = `Last updated: ${timestamp}`;
        })
        .catch(error => {
            console.error('Failed to fetch logs:', error);
            updateLogsStatus('error', 'Failed to load logs');
            showToast('Failed to fetch logs: ' + error.message, 'error');
        })
        .finally(() => {
            LogsViewer.isRefreshing = false;
        });
}

// Display logs in the UI
function displayLogs(data) {
    const logsContent = $('#logs-content');
    if (!logsContent) return;
    
    const logsDisplay = $('#logs-display');
    const wasAtBottom = isScrolledToBottom(logsDisplay);
    
    // Check if data.logs is a string and convert it to an array
    let logsArray = [];
    if (typeof data.logs === 'string') {
        logsArray = data.logs.split('\n');
    } else if (Array.isArray(data.logs)) {
        logsArray = data.logs;
    }

    if (logsArray.length > 0) {
        // Format logs with color coding based on level
        const formattedLogs = logsArray.map(log => formatLogEntry(log)).join('\n');
        logsContent.textContent = formattedLogs;
        
        // Auto-scroll if enabled and was at bottom
        if (LogsViewer.autoScroll && wasAtBottom) {
            scrollToBottom(logsDisplay);
        }
    } else {
        logsContent.textContent = 'No logs available.';
    }
    
    // Update log count if provided
    if (data.total_logs !== undefined) {
        const statusEl = $('#logs-status');
        if (statusEl) {
            const countSpan = statusEl.querySelector('.logs-count') ||
                             document.createElement('span');
            countSpan.className = 'logs-count';
            countSpan.textContent = ` (${data.logs.length}/${data.total_logs} logs)`;
            if (!statusEl.querySelector('.logs-count')) {
                statusEl.appendChild(countSpan);
            }
        }
    }
}

// Format individual log entry
function formatLogEntry(log) {
    // Log format: [timestamp] [level] message
    // Extract components if structured
    const logStr = typeof log === 'string' ? log : log.message || JSON.stringify(log);
    
    // Try to parse ESP-IDF log format: I (timestamp) tag: message
    const espLogPattern = /^([EWIDV])\s+\((\d+)\)\s+([^:]+):\s+(.*)$/;
    const match = logStr.match(espLogPattern);
    
    if (match) {
        const [, level, timestamp, tag, message] = match;
        const levelMap = {
            'E': 'ERROR',
            'W': 'WARN ',
            'I': 'INFO ',
            'D': 'DEBUG',
            'V': 'TRACE'
        };
        return `[${timestamp.padStart(8)}] [${levelMap[level] || level}] ${tag}: ${message}`;
    }
    
    // Return as-is if doesn't match expected format
    return logStr;
}

// Clear logs
function clearLogs() {
    showConfirmDialog('Clear all logs from the device?', () => {
        const statusEl = $('#logs-status');
        const statusText = statusEl?.querySelector('.status-text');
        if (statusText) statusText.textContent = 'Clearing...';
        
        queueRequest('/api/logs?clear=true', 'DELETE')
            .then(response => response.json())
            .then(data => {
                $('#logs-content').textContent = 'Logs cleared.';
                updateLogsStatus('success', 'Logs cleared');
                showToast('Logs cleared successfully', 'success');
                
                // Refresh to show empty state
                setTimeout(refreshLogs, 500);
            })
            .catch(error => {
                console.error('Failed to clear logs:', error);
                updateLogsStatus('error', 'Failed to clear');
                showToast('Failed to clear logs: ' + error.message, 'error');
            });
    });
}

// Toggle auto-refresh
function toggleAutoRefresh() {
    const checkbox = $('#log-auto-refresh');
    LogsViewer.autoRefresh = checkbox?.checked || false;
    
    if (LogsViewer.autoRefresh) {
        startLogsAutoRefresh();
        showToast('Auto-refresh enabled', 'info');
    } else {
        stopLogsAutoRefresh();
        showToast('Auto-refresh disabled', 'info');
    }
}

// Toggle auto-scroll
function toggleAutoScroll() {
    const checkbox = $('#log-auto-scroll');
    LogsViewer.autoScroll = checkbox?.checked || false;
    
    if (LogsViewer.autoScroll) {
        // Scroll to bottom immediately
        const logsDisplay = $('#logs-display');
        scrollToBottom(logsDisplay);
        showToast('Auto-scroll enabled', 'info');
    } else {
        showToast('Auto-scroll disabled', 'info');
    }
}

// Update line count
function updateLogLineCount() {
    const select = $('#log-line-count');
    LogsViewer.lineCount = parseInt(select?.value || 50);
    
    // Refresh with new line count
    refreshLogs();
}

// Update refresh interval
function updateRefreshInterval() {
    const select = $('#log-refresh-interval');
    LogsViewer.autoRefreshInterval = parseInt(select?.value || 3000);
    
    // Restart auto-refresh with new interval if active
    if (LogsViewer.autoRefresh) {
        stopLogsAutoRefresh();
        startLogsAutoRefresh();
    }
}

// Start auto-refresh
function startLogsAutoRefresh() {
    stopLogsAutoRefresh(); // Clear any existing timer
    
    LogsViewer.autoRefreshTimer = setInterval(() => {
        // Only refresh if logs tab is active
        if (state.activeTab === 'logs-tab') {
            refreshLogs();
        }
    }, LogsViewer.autoRefreshInterval);
}

// Stop auto-refresh
function stopLogsAutoRefresh() {
    if (LogsViewer.autoRefreshTimer) {
        clearInterval(LogsViewer.autoRefreshTimer);
        LogsViewer.autoRefreshTimer = null;
    }
}

// Update logs status indicator
function updateLogsStatus(status, message) {
    const statusEl = $('#logs-status');
    if (!statusEl) return;
    
    const statusIndicator = statusEl.querySelector('.status-indicator');
    const statusText = statusEl.querySelector('.status-text');
    
    if (statusIndicator) {
        statusIndicator.className = `status-indicator ${status}`;
    }
    
    if (statusText) {
        statusText.textContent = message;
    }
}

// Check if scrolled to bottom
function isScrolledToBottom(element) {
    if (!element) return true;
    return element.scrollHeight - element.scrollTop <= element.clientHeight + 5;
}

// Scroll to bottom
function scrollToBottom(element) {
    if (!element) return;
    element.scrollTop = element.scrollHeight;
}

// Initialize logs viewer when tab is opened
const originalSwitchTab2 = window.switchTab;
window.switchTab = function(tabName, skipAnimation) {
    originalSwitchTab2(tabName, skipAnimation);
    
    // Initialize logs viewer when logs tab is opened
    if (tabName === 'logs-tab') {
        if (!LogsViewer.initialized) {
            initializeLogsViewer();
            LogsViewer.initialized = true;
        } else if (LogsViewer.autoRefresh) {
            // Refresh logs when returning to tab
            refreshLogs();
        }
    } else {
        // Stop auto-refresh when leaving logs tab
        if (LogsViewer.initialized && LogsViewer.autoRefresh) {
            // Keep timer running but it will check active tab
        }
    }
};

// Make logs functions available globally
window.refreshLogs = refreshLogs;
window.clearLogs = clearLogs;
window.toggleAutoRefresh = toggleAutoRefresh;
window.toggleAutoScroll = toggleAutoScroll;
window.updateLogLineCount = updateLogLineCount;
window.updateRefreshInterval = updateRefreshInterval;
