// gpx_server.cpp — esp_http_server + POSIX file I/O (ESP-IDF)
// Replaces WebServer/SD_MMC/WiFi.h Arduino dependencies.

#include "gpx/gpx_server.h"
#include "gpx/gpx_loader.h"
#include "gpx/gpx_index.h"
#include "ui/ui_manager.h"
#include "hardware/connectivity/wifi_manager.h"
#include "core/arduino_compat.h"
#include "settings_manager.h"
#include "utils/task_manager.h"

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"

#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace gpx_server {

// AP credentials are loaded from NVS via settings_manager (configurable from Settings > WiFi).
// GPX moved SD -> FFat (ADR-0024); logs stay on SD (dev-mode only, low stakes, and the
// enclosure's disassembly requirement doesn't matter for a page gated on dev_mode anyway).
static const char* GPX_FOLDER  = "/ffat/gpx";
static const char* LOGS_FOLDER = "/sdcard/logs";

static httpd_handle_t g_server   = nullptr;
static bool           g_running  = false;
static bool           g_ap_mode  = false;
static esp_netif_t*   g_ap_netif = nullptr;
static char           g_server_ip[16] = "0.0.0.0";

// ============================================================================
// HTML pages
// Note: upload JS uses raw-body POST with ?filename= (no multipart needed)
// ============================================================================

static const char UPLOAD_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>DRAC OS GPX Upload</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: monospace;
            background: #1a1a1a;
            color: #e0e0e0;
            min-height: 100vh;
            padding: 20px;
        }
        .card {
            background: #2a2a2a;
            border-radius: 8px;
            padding: 24px;
            max-width: 560px;
            margin: 20px auto;
        }
        h1 { color: #00ff00; margin-bottom: 4px; }
        .subtitle { color: #aaa; font-size: 0.9em; margin-bottom: 20px; }
        .nav-links { margin-bottom: 20px; }
        .nav-btn {
            color: #00ff00;
            text-decoration: none;
            border: 1px solid #00aa44;
            padding: 6px 14px;
            border-radius: 4px;
            font-size: 0.85em;
            margin-right: 8px;
            display: inline-block;
        }
        .nav-btn:hover { background: #003311; }
        .upload-area {
            border: 2px dashed #00aa44;
            border-radius: 6px;
            padding: 40px 20px;
            text-align: center;
            background: #1f1f1f;
            cursor: pointer;
            transition: background 0.2s, border-color 0.2s;
        }
        .upload-area:hover, .upload-area.dragover {
            background: #003311;
            border-color: #00ff00;
        }
        .upload-text {
            color: #00ff00;
            font-size: 1em;
            margin-bottom: 6px;
        }
        .upload-hint { color: #666; font-size: 0.85em; }
        input[type="file"] { display: none; }
        .file-list { margin-top: 20px; }
        .file-item {
            background: #1f1f1f;
            border: 1px solid #333;
            padding: 10px 14px;
            border-radius: 4px;
            margin-bottom: 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .file-label { display: flex; flex-direction: column; gap: 2px; flex-grow: 1; min-width: 0; margin-right: 10px; }
        .file-name { color: #e0e0e0; font-size: 0.9em; }
        .file-code { color: #666; font-size: 0.75em; }
        .file-checkbox {
            width: 16px;
            height: 16px;
            accent-color: #00aa44;
            margin-right: 10px;
            flex-shrink: 0;
        }
        .bulk-actions {
            display: flex;
            align-items: center;
            gap: 10px;
            margin-top: 20px;
            margin-bottom: 10px;
        }
        .select-all-label {
            display: flex;
            align-items: center;
            gap: 8px;
            color: #e0e0e0;
            font-size: 0.85em;
            cursor: pointer;
        }
        .download-btn {
            color: #00ff00;
            text-decoration: none;
            background: none;
            border: 1px solid #00aa44;
            padding: 4px 10px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.8em;
            font-family: monospace;
            margin-right: 6px;
        }
        .download-btn:hover { background: #003311; }
        .download-btn:disabled {
            color: #666;
            border-color: #444;
            cursor: default;
            background: none;
        }
        .delete-btn {
            color: #ff4444;
            background: none;
            border: 1px solid #aa2222;
            padding: 4px 10px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.8em;
            font-family: monospace;
        }
        .delete-btn:hover { background: #220000; }
        .delete-btn:disabled {
            color: #666;
            border-color: #444;
            cursor: default;
            background: none;
        }
        .status {
            margin-top: 14px;
            padding: 10px 14px;
            border-radius: 4px;
            font-size: 0.9em;
            display: none;
        }
        .status.success {
            background: #003311;
            border: 1px solid #00aa44;
            color: #00ff00;
            display: block;
        }
        .status.error {
            background: #220000;
            border: 1px solid #aa2222;
            color: #ff4444;
            display: block;
        }
        .progress {
            margin-top: 14px;
            padding: 10px 14px;
            border-radius: 4px;
            font-size: 0.9em;
            background: #1f1f1f;
            border: 1px solid #00aa44;
            color: #00ff00;
            display: none;
        }
        .progress.active { display: block; }
        .progress .dots::after {
            content: '';
            animation: progress-dots 1.2s steps(4, end) infinite;
        }
        @keyframes progress-dots {
            0%   { content: ''; }
            25%  { content: '.'; }
            50%  { content: '..'; }
            75%  { content: '...'; }
            100% { content: ''; }
        }
        .info-box {
            background: #1f1f1f;
            border-left: 3px solid #00aa44;
            padding: 10px 14px;
            margin-top: 12px;
            margin-bottom: 20px;
            border-radius: 4px;
            font-size: 0.85em;
            color: #aaa;
        }
        .info-box strong { color: #00ff00; }
        .storage-status {
            background: #1f1f1f;
            border: 1px solid #333;
            padding: 10px 14px;
            border-radius: 4px;
            margin-top: 12px;
            font-size: 0.9em;
            color: #e0e0e0;
        }
        .storage-row {
            display: flex;
            justify-content: space-between;
            margin-bottom: 6px;
        }
        .storage-bar-track {
            background: #111;
            border-radius: 4px;
            height: 10px;
            overflow: hidden;
        }
        .storage-bar-fill {
            height: 100%;
            width: 0%;
            background: #00aa44;
            transition: width 0.3s, background 0.3s;
        }
    </style>
</head>
<body>
    <div class="card">
        <h1>GPX Waypoint Upload</h1>    
        <p class="subtitle">DRAC OS )rawliteral" FW_VERSION R"rawliteral(</p>
        

        <div class="nav-links">
            <a href="/logs" class="nav-btn" id="logsNavLink">System Logs</a>
            <a href="/update" class="nav-btn">Firmware Update</a>
        </div>

        <div class="storage-status">
            <div class="storage-row">
                <span>Flash storage (GPX)</span>
                <span id="storageText" data-loading="1">loading</span>
            </div>
            <div class="storage-bar-track">
                <div class="storage-bar-fill" id="storageBar"></div>
            </div>
            <div class="storage-row" style="margin-top:8px; margin-bottom:0;">
                <span>GPX files</span>
                <span id="fileCountText" data-loading="1">loading</span>
            </div>
        </div>

        <div class="info-box">
            <strong>Auto-load:</strong> Files are loaded automatically when uploaded.
        </div>

        <div class="info-box">
            <strong>Note:</strong> The display will show brief visual interference during each file
            upload or delete — a side effect of writing to flash storage, not a malfunction.
        </div>

        <div class="upload-area" id="uploadArea">
            <div class="upload-text">Drop GPX files here or click to browse</div>
            <div class="upload-hint">Accepts .gpx files only</div>
            <input type="file" id="fileInput" accept=".gpx" multiple>
        </div>

        <div class="status" id="status"></div>
        <div class="progress" id="progress"><span id="progressText"></span><span class="dots"></span></div>

        <div class="bulk-actions" id="bulkActions" style="display:none;">
            <label class="select-all-label">
                <input type="checkbox" id="selectAll" class="file-checkbox" onchange="toggleSelectAll()">
                Select all
            </label>
            <button class="download-btn" id="downloadSelectedBtn" disabled onclick="downloadSelected()">Download Selected</button>
            <button class="delete-btn" id="deleteSelectedBtn" disabled onclick="deleteSelected()">Delete Selected</button>
        </div>

        <div class="file-list" id="fileList"></div>
    </div>

    <script>
        const uploadArea = document.getElementById('uploadArea');
        const fileInput = document.getElementById('fileInput');
        const status = document.getElementById('status');
        const fileList = document.getElementById('fileList');
        const bulkActions = document.getElementById('bulkActions');
        const selectAllBox = document.getElementById('selectAll');
        const downloadSelectedBtn = document.getElementById('downloadSelectedBtn');
        const deleteSelectedBtn = document.getElementById('deleteSelectedBtn');

        // Animated "loading" ellipsis for storageText/fileCountText — cycles
        // ./../... every 400ms while data-loading="1" so a slow /list or /storage
        // response doesn't look stalled. Cleared (data-loading="0") the moment
        // either element gets real content; the interval just no-ops on it after.
        (function animateLoadingDots() {
            const targets = ['storageText', 'fileCountText'];
            let n = 0;
            setInterval(() => {
                n = (n % 3) + 1;
                const dots = '.'.repeat(n);
                for (const id of targets) {
                    const el = document.getElementById(id);
                    if (el.dataset.loading === '1') el.textContent = 'loading' + dots;
                }
            }, 400);
        })();

        // Load existing files on page load
        loadFileList();
        loadStorageInfo();
        applyDevModeUI();

        // Click to browse
        uploadArea.addEventListener('click', () => fileInput.click());

        // File input change
        fileInput.addEventListener('change', (e) => {
            handleFiles(e.target.files);
        });

        // Drag and drop
        uploadArea.addEventListener('dragover', (e) => {
            e.preventDefault();
            uploadArea.classList.add('dragover');
        });

        uploadArea.addEventListener('dragleave', () => {
            uploadArea.classList.remove('dragover');
        });

        uploadArea.addEventListener('drop', (e) => {
            e.preventDefault();
            uploadArea.classList.remove('dragover');
            handleFiles(e.dataTransfer.files);
        });

        // Rebuilds the on-device index once. upload_handler()/delete_handler()
        // no longer do this per-request (see gpx_server.cpp's reload_handler
        // comment) — a batch of N files used to cost O(N^2) because every one
        // of the N requests reloaded from scratch. Call this once after a
        // batch instead, not from inside a per-file loop.
        async function triggerReload() {
            try {
                await fetch('/reload', { method: 'POST' });
            } catch (error) {
                showStatus(`! Reload error: ${error.message}`, 'error');
            }
        }

        async function handleFiles(files) {
            let uploaded = 0, failed = 0, i = 0;
            const total = files.length;
            for (let file of files) {
                i++;
                if (!file.name.toLowerCase().endsWith('.gpx')) {
                    showStatus('Only .gpx files are allowed', 'error');
                    continue;
                }

                showProgress(`Uploading ${i} / ${total}: ${file.name}`);
                try {
                    const response = await fetch('/upload?filename=' + encodeURIComponent(file.name), {
                        method: 'POST',
                        body: file,
                        headers: { 'Content-Type': 'application/octet-stream' }
                    });

                    if (response.ok) {
                        uploaded++;
                    } else {
                        failed++;
                        const text = await response.text();
                        showStatus(`! ${file.name} failed: ${text}`, 'error');
                    }
                } catch (error) {
                    failed++;
                    showStatus(`! ${file.name} error: ${error.message}`, 'error');
                }
            }

            if (uploaded > 0) {
                showProgress('Rebuilding index');
                await triggerReload();
            }
            hideProgress();
            if (failed === 0) {
                showStatus(`+ ${uploaded} file(s) uploaded successfully`, 'success');
            } else if (uploaded === 0) {
                showStatus(`! ${failed} file(s) failed to upload`, 'error');
            } else {
                showStatus(`! ${uploaded} uploaded, ${failed} failed`, 'error');
            }
            loadFileList();
            loadStorageInfo();
        }

        // Shared by loadFileList() (current count) and loadStorageInfo() (max,
        // from gpx_index::MAX_INDEX_FILES) — independent async calls, so the
        // display just reflects whichever pair of values has arrived so far.
        let gFileCount = null, gFileMax = null;
        function updateFileCountDisplay() {
            const el = document.getElementById('fileCountText');
            if (gFileCount === null || gFileMax === null) return;  // still loading — dots animation owns the text
            el.dataset.loading = '0';
            el.textContent = `${gFileCount} / ${gFileMax}`;
        }

        async function loadFileList() {
            try {
                const response = await fetch('/list');
                const data = await response.json();

                gFileCount = data.files ? data.files.length : 0;
                updateFileCountDisplay();

                fileList.innerHTML = '';
                if (data.files && data.files.length > 0) {
                    bulkActions.style.display = 'flex';
                    data.files.forEach(entry => {
                        const file = entry.file;
                        const item = document.createElement('div');
                        item.className = 'file-item';
                        const nameHtml = entry.name
                            ? `<span class="file-name">${escapeHtml(entry.name)}</span>
                               <span class="file-code">${escapeHtml(file)}</span>`
                            : `<span class="file-name">${escapeHtml(file)}</span>`;
                        item.innerHTML = `
                            <input type="checkbox" class="file-checkbox item-checkbox" value="${file}" onchange="updateBulkUI()">
                            <div class="file-label">${nameHtml}</div>
                            <div>
                              <a class="download-btn" href="/download/gpx/${encodeURIComponent(file)}" download="${file}">Download</a>
                              <button class="delete-btn" onclick="deleteFile('${file}')">Delete</button>
                            </div>
                        `;
                        fileList.appendChild(item);
                    });
                } else {
                    bulkActions.style.display = 'none';
                }
                selectAllBox.checked = false;
                updateBulkUI();
            } catch (error) {
                console.error('Failed to load file list:', error);
            }
        }

        function itemCheckboxes() {
            return Array.from(document.querySelectorAll('#fileList .item-checkbox'));
        }

        function updateBulkUI() {
            const boxes = itemCheckboxes();
            const checkedCount = boxes.filter(b => b.checked).length;
            downloadSelectedBtn.disabled = checkedCount === 0;
            deleteSelectedBtn.disabled = checkedCount === 0;
            deleteSelectedBtn.textContent = checkedCount > 0
                ? `Delete Selected (${checkedCount})` : 'Delete Selected';
            selectAllBox.checked = boxes.length > 0 && checkedCount === boxes.length;
        }

        function toggleSelectAll() {
            itemCheckboxes().forEach(b => { b.checked = selectAllBox.checked; });
            updateBulkUI();
        }

        function downloadSelected() {
            const filenames = itemCheckboxes().filter(b => b.checked).map(b => b.value);
            filenames.forEach((filename, i) => {
                setTimeout(() => {
                    const a = document.createElement('a');
                    a.href = `/download/gpx/${encodeURIComponent(filename)}`;
                    a.download = filename;
                    document.body.appendChild(a);
                    a.click();
                    a.remove();
                }, i * 400);
            });
        }

        async function deleteSelected() {
            const filenames = itemCheckboxes().filter(b => b.checked).map(b => b.value);
            if (filenames.length === 0) return;
            if (!confirm(`Delete ${filenames.length} selected file(s)?`)) return;

            let failed = 0, deleted = 0, i = 0;
            const total = filenames.length;
            for (const filename of filenames) {
                i++;
                showProgress(`Deleting ${i} / ${total}: ${filename}`);
                try {
                    const response = await fetch(`/delete/${encodeURIComponent(filename)}`, { method: 'DELETE' });
                    if (response.ok) deleted++; else failed++;
                } catch (error) {
                    failed++;
                }
            }

            if (deleted > 0) {
                showProgress('Rebuilding index');
                await triggerReload();
            }
            hideProgress();
            if (failed === 0) {
                showStatus(`+ ${filenames.length} file(s) deleted`, 'success');
            } else {
                showStatus(`! ${failed} of ${filenames.length} deletions failed`, 'error');
            }
            loadFileList();
            loadStorageInfo();
        }

        function escapeHtml(s) {
            const div = document.createElement('div');
            div.textContent = s;
            return div.innerHTML;
        }

        async function deleteFile(filename) {
            if (!confirm(`Delete ${filename}?`)) return;

            try {
                const response = await fetch(`/delete/${encodeURIComponent(filename)}`, {
                    method: 'DELETE'
                });

                if (response.ok) {
                    await triggerReload();
                    showStatus(`+ ${filename} deleted`, 'success');
                    loadFileList();
                    loadStorageInfo();
                } else {
                    showStatus(`! Delete failed`, 'error');
                }
            } catch (error) {
                showStatus(`! Delete error: ${error.message}`, 'error');
            }
        }

        function showStatus(message, type) {
            status.textContent = message;
            status.className = `status ${type}`;
            setTimeout(() => {
                status.style.display = 'none';
            }, 5000);
        }

        // Separate from showStatus() on purpose — showStatus() auto-hides after
        // 5s, which would fight a long-running batch's live updates. Explicit
        // show/hide instead, so it stays visible for the whole operation.
        function showProgress(text) {
            document.getElementById('progressText').textContent = text;
            document.getElementById('progress').classList.add('active');
        }
        function hideProgress() {
            document.getElementById('progress').classList.remove('active');
        }

        async function loadStorageInfo() {
            const bar = document.getElementById('storageBar');
            const text = document.getElementById('storageText');
            try {
                const r = await fetch('/storage');
                const d = await r.json();
                if (d.error) throw new Error(d.error);
                bar.style.width = d.percent + '%';
                bar.style.background = d.percent >= 90 ? '#ff4444' : (d.percent >= 70 ? '#ffaa00' : '#00aa44');
                text.dataset.loading = '0';
                text.textContent = `${d.percent}% used (${formatBytes(d.used)} / ${formatBytes(d.total)})`;
                gFileMax = d.file_max;
                updateFileCountDisplay();
            } catch (e) {
                text.dataset.loading = '0';
                text.textContent = '(unavailable)';
            }
        }

        function formatBytes(bytes) {
            if (bytes === 0) return '0 Bytes';
            const k = 1024;
            const sizes = ['Bytes', 'KB', 'MB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return Math.round(bytes / Math.pow(k, i) * 100) / 100 + ' ' + sizes[i];
        }

        // Logs page is dev_mode-gated server-side (see /logs, /logs-list) — hide
        // the nav link too so normal users don't see a dead-end button.
        async function applyDevModeUI() {
            try {
                const r = await fetch('/dev-status');
                const d = await r.json();
                if (!d.dev_mode) {
                    document.getElementById('logsNavLink').style.display = 'none';
                }
            } catch (e) {
                // If the check itself fails, leave the link as-is rather than guess.
            }
        }
    </script>
</body>
</html>
)rawliteral";

// HTML logs viewer page
static const char LOGS_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Radar System Logs</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: monospace;
            background: #1a1a1a;
            color: #e0e0e0;
            min-height: 100vh;
            padding: 20px;
        }
        .card {
            background: #2a2a2a;
            border-radius: 8px;
            padding: 24px;
            max-width: 560px;
            margin: 20px auto;
        }
        h1 { color: #00ff00; margin-bottom: 4px; }
        .subtitle { color: #aaa; font-size: 0.9em; margin-bottom: 20px; }
        .nav-links { margin-bottom: 20px; }
        .nav-btn {
            color: #00ff00;
            text-decoration: none;
            border: 1px solid #00aa44;
            padding: 6px 14px;
            border-radius: 4px;
            font-size: 0.85em;
            margin-right: 8px;
            display: inline-block;
        }
        .nav-btn:hover { background: #003311; }
        .info-box {
            background: #1f1f1f;
            border-left: 3px solid #00aa44;
            padding: 10px 14px;
            margin-bottom: 20px;
            border-radius: 4px;
            font-size: 0.85em;
            color: #aaa;
        }
        .info-box strong { color: #00ff00; }
        .bulk-actions {
            display: flex;
            align-items: center;
            gap: 10px;
            margin-bottom: 10px;
        }
        .select-all-label {
            display: flex;
            align-items: center;
            gap: 8px;
            color: #e0e0e0;
            font-size: 0.85em;
            cursor: pointer;
        }
        .log-checkbox {
            width: 16px;
            height: 16px;
            accent-color: #00aa44;
            margin-right: 10px;
            flex-shrink: 0;
        }
        .delete-selected-btn {
            color: #ff4444;
            background: none;
            border: 1px solid #aa2222;
            padding: 4px 10px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.8em;
            font-family: monospace;
        }
        .delete-selected-btn:hover { background: #220000; }
        .delete-selected-btn:disabled {
            color: #666;
            border-color: #444;
            cursor: default;
            background: none;
        }
        .log-list { margin-top: 20px; }
        .log-item {
            background: #1f1f1f;
            border: 1px solid #333;
            padding: 10px 14px;
            border-radius: 4px;
            margin-bottom: 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .log-info { flex-grow: 1; min-width: 0; margin-right: 10px; }
        .log-name { color: #e0e0e0; font-size: 0.9em; margin-bottom: 2px; word-break: break-all; }
        .log-size { color: #666; font-size: 0.75em; }
        .log-actions {
            display: flex;
            gap: 6px;
            flex-shrink: 0;
        }
        .download-btn {
            color: #00ff00;
            text-decoration: none;
            background: none;
            border: 1px solid #00aa44;
            padding: 4px 10px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.8em;
            font-family: monospace;
        }
        .download-btn:hover { background: #003311; }
        .download-btn:disabled {
            color: #666;
            border-color: #444;
            cursor: default;
            background: none;
        }
        .delete-btn {
            color: #ff4444;
            background: none;
            border: 1px solid #aa2222;
            padding: 4px 10px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.8em;
            font-family: monospace;
        }
        .delete-btn:hover { background: #220000; }
        .status {
            margin-top: 14px;
            padding: 10px 14px;
            border-radius: 4px;
            font-size: 0.9em;
            display: none;
        }
        .status.success {
            background: #003311;
            border: 1px solid #00aa44;
            color: #00ff00;
            display: block;
        }
        .status.error {
            background: #220000;
            border: 1px solid #aa2222;
            color: #ff4444;
            display: block;
        }
        .empty-state {
            text-align: center;
            padding: 40px;
            color: #666;
        }
    </style>
</head>
<body>
    <div class="card">
        <h1>System Logs</h1>
        <p class="subtitle">View and download system log files</p>

        <div class="nav-links">
            <a href="/" class="nav-btn">&larr; Back to GPX Upload</a>
        </div>

        <div class="info-box">
            <strong>Log Files:</strong> This page is only reachable while dev mode is on. Log
            files are stored on the physical SD card (not internal flash) and contain boot events,
            GPS data, time sync info, and diagnostic information — GPX waypoint files, by contrast,
            live on internal flash storage. Download logs for debugging or analysis before turning
            dev mode back off.
        </div>

        <div class="status" id="status"></div>

        <div class="bulk-actions" id="bulkActions" style="display:none;">
            <label class="select-all-label">
                <input type="checkbox" id="selectAll" class="log-checkbox" onchange="toggleSelectAll()">
                Select all
            </label>
            <button class="download-btn" id="downloadSelectedBtn" disabled onclick="downloadSelected()">Download Selected</button>
            <button class="delete-selected-btn" id="deleteSelectedBtn" disabled onclick="deleteSelected()">Delete Selected</button>
        </div>

        <div class="log-list" id="logList">
            <div class="empty-state">Loading...</div>
        </div>
    </div>

    <script>
        const status = document.getElementById('status');
        const logList = document.getElementById('logList');
        const bulkActions = document.getElementById('bulkActions');
        const selectAllBox = document.getElementById('selectAll');
        const downloadSelectedBtn = document.getElementById('downloadSelectedBtn');
        const deleteSelectedBtn = document.getElementById('deleteSelectedBtn');

        // Load log files on page load
        loadLogList();

        async function loadLogList() {
            try {
                const response = await fetch('/logs-list');
                const data = await response.json();

                logList.innerHTML = '';
                if (data.files && data.files.length > 0) {
                    bulkActions.style.display = 'flex';
                    data.files.forEach(file => {
                        const item = document.createElement('div');
                        item.className = 'log-item';
                        item.innerHTML = `
                            <input type="checkbox" class="log-checkbox item-checkbox" value="${file.name}" onchange="updateBulkUI()">
                            <div class="log-info">
                                <div class="log-name">${file.name}</div>
                                <div class="log-size">${formatBytes(file.size)}</div>
                            </div>
                            <div class="log-actions">
                                <button class="download-btn" onclick="downloadLog('${file.name}')">Download</button>
                                <button class="delete-btn" onclick="deleteLog('${file.name}')">Delete</button>
                            </div>
                        `;
                        logList.appendChild(item);
                    });
                } else {
                    bulkActions.style.display = 'none';
                    logList.innerHTML = '<div class="empty-state">No log files found</div>';
                }
                selectAllBox.checked = false;
                updateBulkUI();
            } catch (error) {
                console.error('Failed to load log list:', error);
                logList.innerHTML = '<div class="empty-state">Error loading logs</div>';
            }
        }

        function itemCheckboxes() {
            return Array.from(document.querySelectorAll('.item-checkbox'));
        }

        function updateBulkUI() {
            const boxes = itemCheckboxes();
            const checkedCount = boxes.filter(b => b.checked).length;
            downloadSelectedBtn.disabled = checkedCount === 0;
            deleteSelectedBtn.disabled = checkedCount === 0;
            deleteSelectedBtn.textContent = checkedCount > 0
                ? `Delete Selected (${checkedCount})` : 'Delete Selected';
            selectAllBox.checked = boxes.length > 0 && checkedCount === boxes.length;
        }

        function toggleSelectAll() {
            itemCheckboxes().forEach(b => { b.checked = selectAllBox.checked; });
            updateBulkUI();
        }

        function downloadLog(filename) {
            window.location.href = `/download/logs/${filename}`;
        }

        function downloadSelected() {
            const filenames = itemCheckboxes().filter(b => b.checked).map(b => b.value);
            filenames.forEach((filename, i) => {
                setTimeout(() => {
                    const a = document.createElement('a');
                    a.href = `/download/logs/${encodeURIComponent(filename)}`;
                    a.download = filename;
                    document.body.appendChild(a);
                    a.click();
                    a.remove();
                }, i * 400);
            });
        }

        async function deleteLog(filename) {
            if (!confirm(`Delete ${filename}?`)) return;

            try {
                const response = await fetch(`/delete/logs/${filename}`, {
                    method: 'DELETE'
                });

                if (response.ok) {
                    showStatus(`+ ${filename} deleted`, 'success');
                    loadLogList();
                } else {
                    showStatus(`! Delete failed`, 'error');
                }
            } catch (error) {
                showStatus(`! Delete error: ${error.message}`, 'error');
            }
        }

        async function deleteSelected() {
            const filenames = itemCheckboxes().filter(b => b.checked).map(b => b.value);
            if (filenames.length === 0) return;
            if (!confirm(`Delete ${filenames.length} selected log file(s)?`)) return;

            let failed = 0;
            for (const filename of filenames) {
                try {
                    const response = await fetch(`/delete/logs/${filename}`, { method: 'DELETE' });
                    if (!response.ok) failed++;
                } catch (error) {
                    failed++;
                }
            }

            if (failed === 0) {
                showStatus(`+ ${filenames.length} log file(s) deleted`, 'success');
            } else {
                showStatus(`! ${failed} of ${filenames.length} deletions failed`, 'error');
            }
            loadLogList();
        }

        function formatBytes(bytes) {
            if (bytes === 0) return '0 Bytes';
            const k = 1024;
            const sizes = ['Bytes', 'KB', 'MB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return Math.round(bytes / Math.pow(k, i) * 100) / 100 + ' ' + sizes[i];
        }

        function showStatus(message, type) {
            status.textContent = message;
            status.className = `status ${type}`;
            setTimeout(() => {
                status.style.display = 'none';
            }, 5000);
        }
    </script>
</body>
</html>
)rawliteral";

// ============================================================================
// OTA firmware update page HTML
// ============================================================================

static const char OTA_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>DRAC OS Firmware Update</title>"
    "<style>"
    "body{font-family:monospace;background:#1a1a1a;color:#e0e0e0;margin:0;padding:20px}"
    "h1{color:#00ff00}h2{color:#aaa;font-size:1em;margin-top:-10px}"
    ".card{background:#2a2a2a;border-radius:8px;padding:20px;max-width:480px;margin:20px auto}"
    "input[type=file]{width:100%;padding:10px;background:#333;border:1px solid #555;"
    "color:#e0e0e0;border-radius:4px;box-sizing:border-box;cursor:pointer}"
    "button{margin-top:14px;width:100%;padding:12px;background:#00aa44;border:none;"
    "color:#fff;border-radius:4px;font-size:1em;cursor:pointer}"
    "button:disabled{background:#555;cursor:default}"
    "#progress{margin-top:12px;height:8px;background:#333;border-radius:4px;overflow:hidden}"
    "#bar{height:100%;width:0%;background:#00ff00;transition:width 0.2s}"
    "#status{margin-top:10px;font-size:0.9em;color:#aaa}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>DRAC OS</h1>"
    "<h2>Firmware Update &mdash; current: " FW_VERSION "</h2>"
    "<a href='/' style='font-size:0.85em;color:#666;text-decoration:none;'>&#8592; Back to GPX Upload</a><br><br>"
    "<div style='background:#2a1a00;border:1px solid #ff8800;border-radius:4px;"
    "padding:10px;margin-bottom:14px;font-size:0.85em;color:#ffaa44;'>"
    "&#9888; The display will show interference during upload &mdash; this is normal. "
    "Do not close this page or power off the device until it reboots."
    "</div>"
    "<input type='file' id='fw' accept='.bin'><br>"
    "<button id='btn' onclick='upload()'>Flash firmware</button>"
    "<div id='progress'><div id='bar'></div></div>"
    "<div id='status'>Select a .bin file to begin.</div>"
    "</div>"
    "<script>"
    "function upload(){"
    "  var f=document.getElementById('fw').files[0];"
    "  if(!f){document.getElementById('status').textContent='No file selected.';return;}"
    "  var btn=document.getElementById('btn');"
    "  btn.disabled=true;"
    "  var xhr=new XMLHttpRequest();"
    "  xhr.upload.onprogress=function(e){"
    "    var pct=Math.round(e.loaded/e.total*100);"
    "    document.getElementById('bar').style.width=pct+'%';"
    "    document.getElementById('status').textContent='Uploading... '+pct+'%';"
    "  };"
    "  xhr.onload=function(){"
    "    if(xhr.status===200){"
    "      document.body.innerHTML="
    "        '<div style=\"font-family:monospace;background:#1a1a1a;color:#00ff00;"
    "min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "flex-direction:column;text-align:center;padding:20px\">"
    "<h1>DRAC OS</h1>"
    "<p style=\"font-size:1.2em\">Flash complete.</p>"
    "<p style=\"color:#aaa\">Device is rebooting &mdash; this page will not reconnect automatically.<br>"
    "Close this tab and reconnect to the device when it is back online.</p>"
    "</div>';"
    "    } else {"
    "      document.getElementById('status').textContent='Error: '+xhr.responseText;"
    "      document.getElementById('bar').style.background='#ff4444';"
    "      btn.disabled=false;"
    "    }"
    "  };"
    "  xhr.onerror=function(){"
    "    document.getElementById('status').textContent='Connection lost during upload.';"
    "    btn.disabled=false;"
    "  };"
    "  xhr.open('POST','/update');"
    "  xhr.setRequestHeader('Content-Type','application/octet-stream');"
    "  xhr.send(f);"
    "}"
    "</script></body></html>";

// ============================================================================
// Security helper
// ============================================================================

static bool is_safe_filename(const char* name) {
    if (!name || *name == '\0') return false;
    if (strstr(name, "..") != nullptr) return false;
    if (strchr(name, '/') != nullptr) return false;
    return true;
}

// Appends src to dst (which must already be null-terminated) with JSON string
// escaping. Used for the GPX friendly name below, which is freeform text pulled
// out of the file, not a filename — control chars and quotes are possible.
static void json_escape_append(char* dst, size_t dst_size, const char* src) {
    size_t di = strlen(dst);
    for (const char* p = src; *p && di + 2 < dst_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (di + 3 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20) {
            dst[di++] = ' ';  // collapse control chars rather than emit invalid JSON
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

// Pulls the human-friendly cache name out of a GPX file for display next to the
// filename, which is normally just a bare geocache code (e.g. "GC38EVJ.gpx") and
// tells the user nothing about which cache it is. One geocache per file: prefers
// <groundspeak:name> (the real title), falls back to the waypoint's own <name>
// (the GC code itself) if there's no groundspeak extension block. Line-scoped like
// gpx_loader's parser, but deliberately simpler/standalone — this only needs the
// name, not the full waypoint commit machinery.
static void extractGpxName(const char* filepath, char* out, size_t out_size) {
    out[0] = '\0';
    FILE* f = fopen(filepath, "r");
    if (!f) return;

    char line[256];
    char plain_name[64] = {0};
    bool in_wpt = false;
    bool found_groundspeak = false;

    // Name tags appear near the top of a single-waypoint geocaching GPX file —
    // cap the scan so a large description/log text doesn't cost a full read.
    for (int i = 0; i < 200 && !found_groundspeak; i++) {
        if (!fgets(line, sizeof(line), f)) break;

        if (!in_wpt) {
            if (strstr(line, "<wpt") && strstr(line, "lat=")) in_wpt = true;
            continue;
        }

        const char* gs_start = strstr(line, "<groundspeak:name>");
        if (gs_start) {
            gs_start += 18;  // strlen("<groundspeak:name>") — matches gpx_loader.cpp's parser
            const char* gs_end = strstr(gs_start, "</groundspeak:name>");
            if (gs_end) {
                size_t len = (size_t)(gs_end - gs_start);
                if (len >= out_size) len = out_size - 1;
                strncpy(out, gs_start, len);
                out[len] = '\0';
                found_groundspeak = true;
            }
            continue;
        }

        if (plain_name[0] == '\0' && strstr(line, "<name>") && !strstr(line, "groundspeak")) {
            const char* n_start = strstr(line, "<name>");
            n_start += 6;
            const char* n_end = strstr(n_start, "</name>");
            if (n_end) {
                size_t len = (size_t)(n_end - n_start);
                if (len >= sizeof(plain_name)) len = sizeof(plain_name) - 1;
                strncpy(plain_name, n_start, len);
                plain_name[len] = '\0';
            }
        }
    }
    fclose(f);

    if (!found_groundspeak && plain_name[0] != '\0') {
        strncpy(out, plain_name, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

// ============================================================================
// HTTP handlers
// ============================================================================

static esp_err_t send_html_chunked(httpd_req_t* req, const char* html) {
    const size_t CHUNK = 1024;
    const char* p = html;
    size_t remaining = strlen(html);
    while (remaining > 0) {
        size_t n = remaining < CHUNK ? remaining : CHUNK;
        if (httpd_resp_send_chunk(req, p, (ssize_t)n) != ESP_OK) {
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
        p += n;
        remaining -= n;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t ota_page_handler(httpd_req_t* req) {
    Serial.println("[OTA] GET /update - firmware update page");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return send_html_chunked(req, OTA_HTML);
}

// One-shot guard — resets on reboot. Prevents a browser retry or stale tab from
// re-flashing the device after a successful OTA without user intent.
static bool ota_already_triggered = false;

static esp_err_t ota_upload_handler(httpd_req_t* req) {
    if (ota_already_triggered) {
        Serial.println("[OTA] BLOCKED: second POST ignored — OTA already completed this session");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "OTA already completed. Device is rebooting or has rebooted.");
        return ESP_FAIL;
    }
    ota_already_triggered = true;

    Serial.printf("[OTA] POST /update - receiving firmware (%d bytes)\n", req->content_len);

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(nullptr);
    if (!update_part) {
        Serial.println("[OTA] ERROR: no OTA partition found — check partitions_ota.csv");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    Serial.printf("[OTA] Writing to partition: %s at 0x%x\n",
                  update_part->label, update_part->address);

    esp_ota_handle_t ota_handle = 0;
    if (esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        Serial.println("[OTA] ERROR: esp_ota_begin failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    int written   = 0;
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, (size_t)chunk);
        if (n <= 0) {
            Serial.println("[OTA] ERROR: receive error or timeout");
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota_handle, buf, (size_t)n) != ESP_OK) {
            Serial.println("[OTA] ERROR: esp_ota_write failed");
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        remaining -= n;
        written   += n;
        if ((written % (64 * 1024)) == 0) {
            Serial.printf("[OTA] Progress: %d / %d bytes\n", written, req->content_len);
        }
    }

    if (esp_ota_end(ota_handle) != ESP_OK) {
        Serial.println("[OTA] ERROR: esp_ota_end failed (bad image?)");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed (bad image)");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(update_part) != ESP_OK) {
        Serial.println("[OTA] ERROR: esp_ota_set_boot_partition failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    Serial.printf("[OTA] Success - %d bytes flashed. Rebooting into radar mode...\n", written);

    // Invalidate stamp + clear WiFi boot flags atomically.
    // Writing stamp=0 guarantees a mismatch on next boot (FW_BUILD_TS is always > 0),
    // so the mismatch handler in loadSettings() always clears the flags — even if the
    // device had WiFi mode enabled before the update.
    settings_manager::prepareForOTAReboot();

    httpd_resp_send(req, "OK", 2);
    vTaskDelay(pdMS_TO_TICKS(500));  // Let the HTTP response flush before reset
    esp_restart();
    return ESP_OK;  // unreachable, but satisfies compiler
}

static esp_err_t root_handler(httpd_req_t* req) {
    Serial.println("[GPX_SERVER] GET / - upload page");
    httpd_resp_set_type(req, "text/html");
    return send_html_chunked(req, UPLOAD_HTML);
}

static esp_err_t upload_handler(httpd_req_t* req) {
    // Filename comes from ?filename= query param; body is raw file bytes
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
        return ESP_FAIL;
    }
    char filename[128];
    if (httpd_query_key_value(query, "filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename param");
        return ESP_FAIL;
    }

    if (!is_safe_filename(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    size_t len = strlen(filename);
    if (len < 5 || strcasecmp(filename + len - 4, ".gpx") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Only .gpx files allowed");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > 5 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, filename);

    FILE* f = fopen(filepath, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    char buf[512];
    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int chunk = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, (size_t)chunk);
        if (n <= 0) {
            fclose(f);
            remove(filepath);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        fwrite(buf, 1, (size_t)n, f);
        remaining -= n;
    }
    fclose(f);

    Serial.printf("[GPX_SERVER] Upload OK: %s (%d bytes)\n", filename, (int)req->content_len);

    // Deliberately does NOT reload here — see reload_handler()'s comment. The
    // client is responsible for POSTing /reload once after a batch completes.
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// POST /reload — rebuilds the PSRAM index and re-materializes the working set
// against the GPX folder's current contents. upload_handler()/delete_handler()
// used to do this automatically after every single file; that made an
// N-file batch (upload or delete) cost O(N^2) total, since every one of the N
// requests re-scanned every file present at that point — confirmed on
// hardware 2026-08-07 (100 files: 1m22s, next 100: 3m36s, non-linear as
// predicted). The client now calls this once after a batch instead of relying
// on a reload from inside every individual upload/delete. Safe to call any
// number of times, including zero net-new files (e.g. after a failed upload).
static esp_err_t reload_handler(httpd_req_t* req) {
    int reloaded = gpx_loader::refreshGPXFiles();
    Serial.printf("[GPX_SERVER] Manual reload: %d waypoints\n", reloaded);

    task_manager::UIUpdate upd = {};
    upd.type = task_manager::UIUpdateType::RADAR_REFRESH;
    task_manager::queueUIUpdate(upd);

    char buf[48];
    int len = snprintf(buf, sizeof(buf), "{\"waypoints\":%d}", reloaded);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t list_handler(httpd_req_t* req) {
    DIR* dir = opendir(GPX_FOLDER);
    if (!dir) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"files\":[]}", 12);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"files\":[", 10);

    struct dirent* entry;
    bool first = true;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t nl = strlen(name);
        if (nl >= 4 && strcasecmp(name + nl - 4, ".gpx") == 0) {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, name);
            char cache_name[96];
            extractGpxName(filepath, cache_name, sizeof(cache_name));

            char item[384];
            int written = snprintf(item, sizeof(item), "%s{\"file\":\"%s\",\"name\":\"",
                                    first ? "" : ",", name);
            json_escape_append(item, sizeof(item), cache_name);
            written = strlen(item);
            written += snprintf(item + written, sizeof(item) - written, "\"}");
            httpd_resp_send_chunk(req, item, (ssize_t)strlen(item));
            first = false;
        }
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t waypoints_handler(httpd_req_t* req) {
    int count = gpx_loader::getWaypointCount();
    int max   = ui_manager::RadarConfig::MAX_WAYPOINTS;

    char buf[48];
    int len = snprintf(buf, sizeof(buf), "{\"count\":%d,\"max\":%d}", count, max);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t storage_handler(httpd_req_t* req) {
    uint64_t total_bytes = 0, free_bytes = 0;
    httpd_resp_set_type(req, "application/json");

    if (esp_vfs_fat_info("/ffat", &total_bytes, &free_bytes) != ESP_OK) {
        const char* err = "{\"error\":\"unavailable\"}";
        httpd_resp_send(req, err, (ssize_t)strlen(err));
        return ESP_OK;
    }

    uint64_t used_bytes = total_bytes - free_bytes;
    int percent = total_bytes > 0 ? (int)((used_bytes * 100) / total_bytes) : 0;

    char buf[192];
    int len = snprintf(buf, sizeof(buf),
        "{\"total\":%llu,\"free\":%llu,\"used\":%llu,\"percent\":%d,\"file_max\":%d}",
        (unsigned long long)total_bytes, (unsigned long long)free_bytes,
        (unsigned long long)used_bytes, percent, gpx_index::MAX_INDEX_FILES);
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// Lets the upload page's JS know whether to show the System Logs nav link — the
// /logs* routes themselves are the actual gate (see logs_page_handler), this is
// just so a normal user doesn't see a dead-end button.
static esp_err_t dev_status_handler(httpd_req_t* req) {
    bool dev_mode = settings_manager::getSettings().dev_mode;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "{\"dev_mode\":%s}", dev_mode ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// The logs page/API/downloads only exist for dev-mode field debugging — a normal
// user with dev_mode off gets a 404 on all of them, not just a hidden nav link
// (see the JS-side hide in UPLOAD_HTML, which is cosmetic only).
static bool devLoggingUIEnabled() {
    return settings_manager::getSettings().dev_mode;
}

static esp_err_t logs_page_handler(httpd_req_t* req) {
    if (!devLoggingUIEnabled()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_FAIL;
    }
    Serial.println("[GPX_SERVER] GET /logs - logs page");
    httpd_resp_set_type(req, "text/html");
    return send_html_chunked(req, LOGS_HTML);
}

static esp_err_t logs_list_handler(httpd_req_t* req) {
    if (!devLoggingUIEnabled()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_FAIL;
    }
    DIR* dir = opendir(LOGS_FOLDER);
    if (!dir) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"files\":[]}", 12);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"files\":[", 10);

    struct dirent* entry;
    bool first = true;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t nl = strlen(name);
        // .csv as well as .log — field-log samples are CSVs, and without this they
        // exist on the card but are invisible on this page, which is the only way
        // to get them off the device (no serial on battery).
        bool is_log = (nl >= 4 && strcasecmp(name + nl - 4, ".log") == 0);
        bool is_csv = (nl >= 4 && strcasecmp(name + nl - 4, ".csv") == 0);
        if (is_log || is_csv) {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%s", LOGS_FOLDER, name);
            struct stat st;
            long fsize = 0;
            if (stat(filepath, &st) == 0) fsize = (long)st.st_size;

            char item[256];
            int written = snprintf(item, sizeof(item),
                "%s{\"name\":\"%s\",\"size\":%ld}", first ? "" : ",", name, fsize);
            httpd_resp_send_chunk(req, item, (ssize_t)written);
            first = false;
        }
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// Handles DELETE /delete/<filename>  (GPX)
//     and DELETE /delete/logs/<filename>  (log)
static esp_err_t delete_handler(httpd_req_t* req) {
    const char* uri = req->uri;
    char filepath[256];
    const char* filename = nullptr;

    if (strncmp(uri, "/delete/logs/", 13) == 0) {
        if (!devLoggingUIEnabled()) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
            return ESP_FAIL;
        }
        filename = uri + 13;
        if (!is_safe_filename(filename)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
            return ESP_FAIL;
        }
        snprintf(filepath, sizeof(filepath), "%s/%s", LOGS_FOLDER, filename);
    } else if (strncmp(uri, "/delete/", 8) == 0) {
        filename = uri + 8;
        if (!is_safe_filename(filename)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
            return ESP_FAIL;
        }
        snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, filename);
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_FAIL;
    }

    if (access(filepath, F_OK) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    if (remove(filepath) == 0) {
        Serial.printf("[GPX_SERVER] Deleted: %s\n", filepath);

        // Deliberately does NOT reload here anymore (see reload_handler()'s
        // comment) — the client POSTs /reload once after a batch. In the gap,
        // the PSRAM index can hold file_offsets into this now-gone file; that's
        // safe, not just deferred-risky: gpx_loader::reselect()/
        // selectAndMaterialize() already treat a failed fopen() on a stale
        // entry as "drop this one from the working set", not a crash — this
        // predates this change (see their fopen-failure branches).

        httpd_resp_send(req, "Deleted", 7);
        return ESP_OK;
    }

    Serial.printf("[GPX_SERVER] ERROR: delete failed: %s\n", filepath);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    return ESP_FAIL;
}

// Handles GET /download/logs/<filename> and /download/gpx/<filename>
static esp_err_t download_handler(httpd_req_t* req) {
    const char* uri = req->uri;
    const char* folder;
    const char* filename;
    const char* mime;

    if (strncmp(uri, "/download/logs/", 15) == 0) {
        if (!devLoggingUIEnabled()) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
            return ESP_FAIL;
        }
        folder   = LOGS_FOLDER;
        filename = uri + 15;
        mime     = "text/plain";
    } else if (strncmp(uri, "/download/gpx/", 14) == 0) {
        folder   = GPX_FOLDER;
        filename = uri + 14;
        mime     = "application/gpx+xml";
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_FAIL;
    }

    if (!is_safe_filename(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", folder, filename);

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, mime);
    char disposition[192];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    // Heap-allocated: httpd dispatch (~2KB) + lwIP send path (~1KB) + this frame leaves
    // no room for a 512-byte local array within the 4KB httpd task stack.
    char* buf = (char*)malloc(512);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t n;
    while ((n = fread(buf, 1, 512, f)) > 0) {
        httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// ============================================================================
// Public API
// ============================================================================

bool init() {
    Serial.println("[GPX_SERVER] Initializing...");

    struct stat st;
    if (stat(GPX_FOLDER, &st) != 0) {
        Serial.println("[GPX_SERVER] Creating /ffat/gpx...");
        if (mkdir(GPX_FOLDER, 0777) != 0) {
            Serial.println("[GPX_SERVER] ERROR: Failed to create gpx folder");
            return false;
        }
    }

    Serial.println("[GPX_SERVER] Init complete");
    return true;
}

bool start() {
    if (g_running) return true;

    // Use existing STA connection if available, otherwise create AP
    if (wifi_manager::isConnected()) {
        g_ap_mode = false;
        snprintf(g_server_ip, sizeof(g_server_ip), "%s",
                 wifi_manager::getIPAddress().c_str());
        // DHCP guard: isConnected() returns true at assoc→run, ~1s before IP is assigned.
        // Starting httpd with 0.0.0.0 fails silently and leaves a zombie socket on port 80,
        // causing every subsequent attempt to fail with EADDRINUSE. Wait for real IP.
        if (strcmp(g_server_ip, "0.0.0.0") == 0) {
            return false;
        }
        Serial.printf("[GPX_SERVER] Using STA IP: %s\n", g_server_ip);
    } else {
        g_ap_mode = true;
        const char* ap_ssid = settings_manager::getSettings().ap_ssid;
        const char* ap_pass = settings_manager::getSettings().ap_password;
        Serial.printf("[GPX_SERVER] Creating AP: SSID=%s\n", ap_ssid);

        if (!g_ap_netif) {
            g_ap_netif = esp_netif_create_default_wifi_ap();
        }
        // In ESP-IDF, set_config + set_mode don't take effect until WiFi is
        // (re)started. Stop first so the new config is applied on start.
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_AP);

        wifi_config_t ap_cfg = {};
        strncpy((char*)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
        strncpy((char*)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.ssid_len            = (uint8_t)strlen(ap_ssid);
        ap_cfg.ap.channel             = 6;
        ap_cfg.ap.authmode            = WIFI_AUTH_WPA2_WPA3_PSK;  // WPA3 for macOS/iOS, WPA2 fallback for older devices
        ap_cfg.ap.max_connection      = 4;
        ap_cfg.ap.pmf_cfg.capable     = true;
        ap_cfg.ap.pmf_cfg.required    = false;  // WPA3 clients always use PMF; WPA2 clients optional
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        esp_wifi_start();

        snprintf(g_server_ip, sizeof(g_server_ip), "192.168.4.1");
        Serial.println("[GPX_SERVER] AP Password: ******** (see Settings screen)");
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn    = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 15;  // 13 registered below, some headroom
    cfg.lru_purge_enable = true; // evict stuck half-open connections so httpd_start succeeds on retry
    // Default 4096B is too thin for upload_handler() -> refreshGPXFiles() -> parseGPXFile(),
    // which stacks ~2-3KB of local buffers (wp_desc[1024]+line[512]+...) on this same task
    // while also doing SD/FATFS I/O — a large GPX upload triggering a full reload can overflow
    // the default stack. Only allocated while the web server is actually running (see start()
    // below), so this doesn't cost anything in normal radar-mode boot.
    cfg.stack_size = 8192;

    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        Serial.println("[GPX_SERVER] ERROR: httpd_start failed");
        return false;
    }

    const httpd_uri_t uris[] = {
        { "/",           HTTP_GET,    root_handler,       nullptr },
        { "/upload",     HTTP_POST,   upload_handler,     nullptr },
        { "/reload",     HTTP_POST,   reload_handler,     nullptr },
        { "/list",       HTTP_GET,    list_handler,       nullptr },
        { "/waypoints",  HTTP_GET,    waypoints_handler,  nullptr },
        { "/storage",    HTTP_GET,    storage_handler,    nullptr },
        { "/dev-status", HTTP_GET,    dev_status_handler, nullptr },
        { "/logs",       HTTP_GET,    logs_page_handler,  nullptr },
        { "/logs-list",  HTTP_GET,    logs_list_handler,  nullptr },
        { "/delete/*",   HTTP_DELETE, delete_handler,     nullptr },
        { "/download/*", HTTP_GET,    download_handler,   nullptr },
        { "/update",     HTTP_GET,    ota_page_handler,   nullptr },
        { "/update",     HTTP_POST,   ota_upload_handler, nullptr },
    };
    for (const auto& u : uris) {
        httpd_register_uri_handler(g_server, &u);
    }

    g_running = true;
    Serial.printf("[GPX_SERVER] Started at http://%s\n", g_server_ip);
    return true;
}

void stop() {
    if (!g_running) return;

    httpd_stop(g_server);
    g_server = nullptr;

    if (g_ap_mode) {
        esp_wifi_stop();
        // Restore WiFi to dormant state — wifi_manager::setEnabled(true) will
        // set STA mode and create the STA netif if needed.
        g_ap_mode = false;
    }

    g_running = false;
    Serial.println("[GPX_SERVER] Stopped");
}

void handle() {
    // esp_http_server runs in its own FreeRTOS tasks — nothing to poll
}

bool isRunning() { return g_running; }

bool getStatus(char* ip_address, size_t max_len) {
    if (!g_running) return false;
    snprintf(ip_address, max_len, "%s", g_server_ip);
    return true;
}

} // namespace gpx_server
