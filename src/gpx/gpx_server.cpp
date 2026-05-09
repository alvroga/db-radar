// gpx_server.cpp — esp_http_server + POSIX file I/O (ESP-IDF)
// Replaces WebServer/SD_MMC/WiFi.h Arduino dependencies.

#include "gpx/gpx_server.h"
#include "hardware/connectivity/wifi_manager.h"
#include "core/arduino_compat.h"
#include "settings_manager.h"

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace gpx_server {

// AP credentials are loaded from NVS via settings_manager (configurable from Settings > WiFi).
static const char* GPX_FOLDER  = "/sdcard/gpx";
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
        .file-name { color: #e0e0e0; font-size: 0.9em; }
        .download-btn {
            color: #00ff00;
            text-decoration: none;
            border: 1px solid #00aa44;
            padding: 4px 10px;
            border-radius: 4px;
            font-size: 0.8em;
            margin-right: 6px;
        }
        .download-btn:hover { background: #003311; }
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
        .info-box {
            background: #1f1f1f;
            border-left: 3px solid #00aa44;
            padding: 10px 14px;
            margin-top: 20px;
            border-radius: 4px;
            font-size: 0.85em;
            color: #aaa;
        }
        .info-box strong { color: #00ff00; }
    </style>
</head>
<body>
    <div class="card">
        <h1>GPX Waypoint Upload</h1>    
        <p class="subtitle">DRAC OS )rawliteral" FW_VERSION R"rawliteral(</p>
        

        <div class="nav-links">
            <a href="/logs" class="nav-btn">System Logs</a>
            <a href="/update" class="nav-btn">Firmware Update</a>
        </div>

        <div class="upload-area" id="uploadArea">
            <div class="upload-text">Drop GPX files here or click to browse</div>
            <div class="upload-hint">Accepts .gpx files only</div>
            <input type="file" id="fileInput" accept=".gpx" multiple>
        </div>

        <div class="status" id="status"></div>

        <div class="file-list" id="fileList"></div>

        <div class="info-box">
            <strong>Auto-load:</strong> All GPX files in /gpx/ are loaded on boot. Delete files to remove waypoints.
        </div>
    </div>

    <script>
        const uploadArea = document.getElementById('uploadArea');
        const fileInput = document.getElementById('fileInput');
        const status = document.getElementById('status');
        const fileList = document.getElementById('fileList');

        // Load existing files on page load
        loadFileList();

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

        async function handleFiles(files) {
            for (let file of files) {
                if (!file.name.toLowerCase().endsWith('.gpx')) {
                    showStatus('Only .gpx files are allowed', 'error');
                    continue;
                }

                try {
                    const response = await fetch('/upload?filename=' + encodeURIComponent(file.name), {
                        method: 'POST',
                        body: file,
                        headers: { 'Content-Type': 'application/octet-stream' }
                    });

                    if (response.ok) {
                        showStatus(`+ ${file.name} uploaded successfully`, 'success');
                        loadFileList();
                    } else {
                        const text = await response.text();
                        showStatus(`! Upload failed: ${text}`, 'error');
                    }
                } catch (error) {
                    showStatus(`! Upload error: ${error.message}`, 'error');
                }
            }
        }

        async function loadFileList() {
            try {
                const response = await fetch('/list');
                const data = await response.json();

                fileList.innerHTML = '';
                if (data.files && data.files.length > 0) {
                    data.files.forEach(file => {
                        const item = document.createElement('div');
                        item.className = 'file-item';
                        item.innerHTML = `
                            <span class="file-name">${file}</span>
                            <div>
                              <a class="download-btn" href="/download/gpx/${file}" download="${file}">Download</a>
                              <button class="delete-btn" onclick="deleteFile('${file}')">Delete</button>
                            </div>
                        `;
                        fileList.appendChild(item);
                    });
                }
            } catch (error) {
                console.error('Failed to load file list:', error);
            }
        }

        async function deleteFile(filename) {
            if (!confirm(`Delete ${filename}?`)) return;

            try {
                const response = await fetch(`/delete/${filename}`, {
                    method: 'DELETE'
                });

                if (response.ok) {
                    showStatus(`+ ${filename} deleted`, 'success');
                    loadFileList();
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
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 16px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            padding: 40px;
            max-width: 800px;
            margin: 0 auto;
        }
        h1 {
            color: #333;
            margin-bottom: 10px;
            font-size: 28px;
        }
        .subtitle {
            color: #666;
            margin-bottom: 30px;
            font-size: 14px;
        }
        .nav-btn {
            background: #764ba2;
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 8px;
            cursor: pointer;
            font-size: 14px;
            margin-bottom: 20px;
            transition: background 0.2s;
            text-decoration: none;
            display: inline-block;
        }
        .nav-btn:hover {
            background: #667eea;
        }
        .log-list {
            margin-top: 20px;
        }
        .log-item {
            background: #f5f5f5;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 10px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .log-info {
            flex-grow: 1;
        }
        .log-name {
            color: #333;
            font-weight: 500;
            margin-bottom: 5px;
        }
        .log-size {
            color: #999;
            font-size: 12px;
        }
        .log-actions {
            display: flex;
            gap: 10px;
        }
        .download-btn {
            background: #2196F3;
            color: white;
            border: none;
            padding: 8px 16px;
            border-radius: 6px;
            cursor: pointer;
            font-size: 14px;
            transition: background 0.2s;
        }
        .download-btn:hover {
            background: #1976D2;
        }
        .delete-btn {
            background: #ff4757;
            color: white;
            border: none;
            padding: 8px 16px;
            border-radius: 6px;
            cursor: pointer;
            font-size: 14px;
            transition: background 0.2s;
        }
        .delete-btn:hover {
            background: #ff3838;
        }
        .status {
            margin-top: 20px;
            padding: 15px;
            border-radius: 8px;
            display: none;
        }
        .status.success {
            background: #d4edda;
            color: #155724;
            display: block;
        }
        .status.error {
            background: #f8d7da;
            color: #721c24;
            display: block;
        }
        .info-box {
            background: #e7f3ff;
            border-left: 4px solid #2196F3;
            padding: 15px;
            margin-top: 20px;
            border-radius: 4px;
        }
        .info-box strong {
            color: #1976D2;
        }
        .empty-state {
            text-align: center;
            padding: 40px;
            color: #999;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>📋 System Logs</h1>
        <p class="subtitle">View and download system log files</p>

        <a href="/" class="nav-btn">← Back to GPX Upload</a>

        <div class="status" id="status"></div>

        <div class="log-list" id="logList">
            <div class="empty-state">Loading...</div>
        </div>

        <div class="info-box">
            <strong>💾 Log Files:</strong> System logs are stored on the SD card and contain boot events, GPS data, time sync info, and diagnostic information. Download them for debugging or analysis.
        </div>
    </div>

    <script>
        const status = document.getElementById('status');
        const logList = document.getElementById('logList');

        // Load log files on page load
        loadLogList();

        async function loadLogList() {
            try {
                const response = await fetch('/logs-list');
                const data = await response.json();

                logList.innerHTML = '';
                if (data.files && data.files.length > 0) {
                    data.files.forEach(file => {
                        const item = document.createElement('div');
                        item.className = 'log-item';
                        item.innerHTML = `
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
                    logList.innerHTML = '<div class="empty-state">No log files found</div>';
                }
            } catch (error) {
                console.error('Failed to load log list:', error);
                logList.innerHTML = '<div class="empty-state">Error loading logs</div>';
            }
        }

        function downloadLog(filename) {
            window.location.href = `/download/logs/${filename}`;
        }

        async function deleteLog(filename) {
            if (!confirm(`Delete ${filename}?`)) return;

            try {
                const response = await fetch(`/delete/logs/${filename}`, {
                    method: 'DELETE'
                });

                if (response.ok) {
                    showStatus(`✓ ${filename} deleted`, 'success');
                    loadLogList();
                } else {
                    showStatus(`✗ Delete failed`, 'error');
                }
            } catch (error) {
                showStatus(`✗ Delete error: ${error.message}`, 'error');
            }
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
    httpd_resp_send(req, "OK", 2);
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
            char item[192];
            int written = snprintf(item, sizeof(item), "%s\"%s\"", first ? "" : ",", name);
            httpd_resp_send_chunk(req, item, (ssize_t)written);
            first = false;
        }
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t logs_page_handler(httpd_req_t* req) {
    Serial.println("[GPX_SERVER] GET /logs - logs page");
    httpd_resp_set_type(req, "text/html");
    return send_html_chunked(req, LOGS_HTML);
}

static esp_err_t logs_list_handler(httpd_req_t* req) {
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
        if (nl >= 4 && strcasecmp(name + nl - 4, ".log") == 0) {
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
        Serial.println("[GPX_SERVER] Creating /sdcard/gpx...");
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
        Serial.printf("[GPX_SERVER] AP Password: %s\n", ap_pass);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn    = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true; // evict stuck half-open connections so httpd_start succeeds on retry

    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        Serial.println("[GPX_SERVER] ERROR: httpd_start failed");
        return false;
    }

    const httpd_uri_t uris[] = {
        { "/",           HTTP_GET,    root_handler,       nullptr },
        { "/upload",     HTTP_POST,   upload_handler,     nullptr },
        { "/list",       HTTP_GET,    list_handler,       nullptr },
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
