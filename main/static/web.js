let hasRestarted = false;

// 配置缓存，防止工作模式切换时配置丢失
let configCache = {
    lastWorkMode: null
};

const HTTP_REQUEST_TIMEOUT_MS = 10000;
const HTTP_MAX_CONCURRENT_REQUESTS = 2;
const RUNTIME_STATUS_REFRESH_MS = 1000;

let activeHttpRequests = 0;
const pendingHttpRequests = [];

function pumpHttpRequestQueue() {
    while (activeHttpRequests < HTTP_MAX_CONCURRENT_REQUESTS && pendingHttpRequests.length > 0) {
        const item = pendingHttpRequests.shift();
        activeHttpRequests++;
        Promise.resolve()
            .then(item.task)
            .then(item.resolve, item.reject)
            .finally(() => {
                activeHttpRequests--;
                pumpHttpRequestQueue();
            });
    }
}

function queueHttpRequest(task) {
    return new Promise((resolve, reject) => {
        pendingHttpRequests.push({ task, resolve, reject });
        pumpHttpRequestQueue();
    });
}

async function queuedFetch(url, options = {}, timeoutMs = HTTP_REQUEST_TIMEOUT_MS) {
    return queueHttpRequest(async () => {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), timeoutMs);
        const fetchOptions = {
            ...options,
            signal: controller.signal
        };

        try {
            return await fetch(url, fetchOptions);
        } finally {
            clearTimeout(timeoutId);
        }
    });
}

function tr(key, fallback = '') {
    if (window.i18n && typeof window.i18n.t === 'function') {
        return window.i18n.t(key, fallback);
    }
    return fallback || key;
}

function trFormat(key, params = {}, fallback = '') {
    return tr(key, fallback).replace(/\{(\w+)\}/g, (_, name) => {
        return params[name] !== undefined ? params[name] : '';
    });
}

function setI18nText(target, key, fallback = '') {
    const element = typeof target === 'string' ? document.getElementById(target) : target;
    if (!element) return;

    const textTarget = element.querySelector('.btn-container') || element;
    textTarget.textContent = tr(key, fallback);
}

function setI18nFormatText(target, key, params = {}, fallback = '') {
    const element = typeof target === 'string' ? document.getElementById(target) : target;
    if (!element) return;

    const textTarget = element.querySelector('.btn-container') || element;
    textTarget.textContent = trFormat(key, params, fallback);
}

function setRuntimeStatusText(target, key, fallback = '', params = {}) {
    const element = typeof target === 'string' ? document.getElementById(target) : target;
    if (!element) return;

    element.dataset.i18nStatusKey = key;
    element.dataset.i18nStatusFallback = fallback;
    element.dataset.i18nStatusParams = JSON.stringify(params || {});
    element.textContent = trFormat(key, params, fallback);
}

function refreshRuntimeStatusTexts() {
    document.querySelectorAll('[data-i18n-status-key]').forEach(element => {
        let params = {};
        try {
            params = JSON.parse(element.dataset.i18nStatusParams || '{}');
        } catch (error) {
            params = {};
        }

        element.textContent = trFormat(
            element.dataset.i18nStatusKey,
            params,
            element.dataset.i18nStatusFallback || ''
        );
    });
}

function getWebSocketClientTag() {
    try {
        const params = new URLSearchParams(window.location.search);
        const queryTag = params.get('clientTag') || params.get('client_tag');
        if (queryTag && queryTag.trim()) {
            const normalized = queryTag.trim();
            localStorage.setItem('wsClientTag', normalized);
            return normalized;
        }
    } catch (error) {
        console.warn('无法解析URL中的clientTag参数:', error);
    }

    try {
        const savedTag = localStorage.getItem('wsClientTag');
        if (savedTag && savedTag.trim()) {
            return savedTag.trim();
        }

        const generatedTag = `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
        localStorage.setItem('wsClientTag', generatedTag);
        return generatedTag;
    } catch (error) {
        console.warn('无法读取本地存储的wsClientTag:', error);
        return `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
    }
}

function buildWebSocketUrl(path) {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = new URL(`${protocol}//${window.location.host}${path}`);
    const tag = getWebSocketClientTag();
    if (tag) {
        url.searchParams.set('client_tag', tag);
    }
    return url.toString();
}

// Basic info API
const basicInfoElementIds = ['host_name', 'uptime', 'net_mode', 'eth_mac', 'eth_ip', 'sta_mac', 'sta_ip', 'is_dhcps', 'gateway_status', 'dns_status', 'ap_status', 'internet_status'];
let basicInfoElements = {};

// 初始化基本信息元素引用
function initBasicInfoElements() {
    if (Object.keys(basicInfoElements).length === 0) {
        console.log('Initializing basic info elements...');
        basicInfoElements = basicInfoElementIds.reduce((obj, id) => {
            const element = document.getElementById(id);
            if (element) {
                obj[id] = element;
                console.log(`✓ Found element: ${id}`);
            } else {
                console.warn(`✗ Element with id '${id}' not found`);
            }
            return obj;
        }, {});
        console.log(`Basic info elements initialized: ${Object.keys(basicInfoElements).length}/${basicInfoElementIds.length}`);
    }
}

function translateNetworkStatus(statusText) {
    const status = statusText || '';

    if (status.includes('已连接') || status.includes('Connected')) {
        if (status.includes('外网模式') || status.includes('Internet')) {
            return {
                text: tr('common.connectedExternal', '已连接 (外网模式)'),
                className: 'connected-external'
            };
        }

        if (status.includes('内网模式') || status.includes('Intranet')) {
            return {
                text: tr('common.connectedInternal', '已连接 (内网模式)'),
                className: 'connected-internal'
            };
        }

        return {
            text: tr('common.connected', '已连接'),
            className: 'connected-external'
        };
    }

    return {
        text: tr('network.notConnected', '未连接'),
        className: 'disconnected'
    };
}

function updateInternetStatusElement(statusText) {
    if (!basicInfoElements.internet_status) return;

    const translatedStatus = translateNetworkStatus(statusText);
    basicInfoElements.internet_status.textContent = translatedStatus.text;

    basicInfoElements.internet_status.classList.remove('network-status', 'connected-external', 'connected-internal', 'disconnected');
    basicInfoElements.internet_status.classList.add('network-status', translatedStatus.className);
}

function formatSecondsText(seconds) {
    const totalSeconds = Number(seconds) || 0;
    if (totalSeconds <= 0) {
        return trFormat('basicInfo.seconds', { count: 0 }, '0秒');
    }
    if (totalSeconds >= 3600 && totalSeconds % 3600 === 0) {
        return trFormat('basicInfo.hours', { count: totalSeconds / 3600 }, `${totalSeconds / 3600}小时`);
    }
    if (totalSeconds >= 60 && totalSeconds % 60 === 0) {
        return trFormat('basicInfo.minutes', { count: totalSeconds / 60 }, `${totalSeconds / 60}分钟`);
    }
    return trFormat('basicInfo.seconds', { count: totalSeconds }, `${totalSeconds}秒`);
}

function updateApStatusElement(data) {
    if (!basicInfoElements.ap_status || !data) return;

    if (data.ap_state === 'ON') {
        basicInfoElements.ap_status.textContent = Number(data.ap_wait_time_sec) === 0 ?
            tr('basicInfo.apAlwaysOn', '开启（永远开启）') :
            trFormat(
                'basicInfo.apOnRemaining',
                { time: formatSecondsText(data.ap_remaining_sec) },
                `开启（剩余${formatSecondsText(data.ap_remaining_sec)}）`
            );
    } else {
        basicInfoElements.ap_status.textContent = tr('basicInfo.apOff', '已关闭');
    }
}

let basicInfoFetchInFlight = false;

async function updateBasicInfo() {
    if (basicInfoFetchInFlight) return;
    basicInfoFetchInFlight = true;
    try {
        // 确保DOM元素已初始化
        initBasicInfoElements();
        
        const response = await queuedFetch('/devinfo');
        if (!response.ok) {
            console.error('Failed to fetch data. Status:', response.status);
            return;
        }
        const data = await response.json();
        
        // 安全地更新元素内容，检查元素是否存在
        if (basicInfoElements.host_name) {
            basicInfoElements.host_name.textContent = data.host_names ||
                tr('basicInfo.deviceName', '以太网串口服务器');
        }
        
        // 安全地更新系统时间
        if (basicInfoElements.uptime) {
            const now = new Date();
            const timeString = now.getFullYear() + '-' +
                              String(now.getMonth() + 1).padStart(2, '0') + '-' +
                              String(now.getDate()).padStart(2, '0') + ' ' +
                              String(now.getHours()).padStart(2, '0') + ':' +
                              String(now.getMinutes()).padStart(2, '0') + ':' +
                              String(now.getSeconds()).padStart(2, '0');
            basicInfoElements.uptime.textContent = timeString;
        }
        
        // 安全地更新其他基本信息
        if (basicInfoElements.eth_mac) {
            basicInfoElements.eth_mac.textContent = data.device_eth_mac || '';
        }
        if (basicInfoElements.eth_ip) {
            basicInfoElements.eth_ip.textContent = data.eth_ip || '';
        }
        if (basicInfoElements.sta_mac) {
            basicInfoElements.sta_mac.textContent = data.device_sta_mac || '';
        }
        if (basicInfoElements.is_dhcps) {
            basicInfoElements.is_dhcps.textContent = data.is_dhcp == 2 ?
                tr('network.staticIP', 'STATIC(静态IP)') :
                tr('network.dhcp', 'DHCP(动态IP)');
        }
        if (basicInfoElements.net_mode) {
            if (data.network_if === 'WiFi') {
                basicInfoElements.net_mode.textContent = tr('network.wifiMode', 'WIFI(无线连接)');
            } else if (data.network_if === 'Ethernet') {
                basicInfoElements.net_mode.textContent = tr('network.ethernet', '以太网(有线连接)');
            } else {
                basicInfoElements.net_mode.textContent = data.netconn == 2 ?
                    tr('network.wifiMode', 'WIFI(无线连接)') :
                    tr('network.ethernet', '以太网(有线连接)');
            }
        }
        if (basicInfoElements.sta_ip) {
            basicInfoElements.sta_ip.textContent = data.sta_ip || '';
        }
        if (basicInfoElements.gateway_status) {
            basicInfoElements.gateway_status.textContent = data.gateway || '---';
        }
        if (basicInfoElements.dns_status) {
            basicInfoElements.dns_status.textContent = data.dns || '---';
        }
        updateApStatusElement(data);
        updateInternetStatusElement(data.internet_status);
    } catch (error) {
        console.error('Failed to fetch basic info data:', error);
        // 如果元素未找到，可能是DOM还未完全加载，延迟重试
        if (Object.keys(basicInfoElements).length === 0) {
            console.log('DOM elements not ready, retrying in 500ms...');
            setTimeout(() => {
                updateBasicInfo();
            }, 500);
        }
    } finally {
        basicInfoFetchInFlight = false;
    }
}

// 延迟初始化基本信息，确保DOM已加载
setTimeout(() => {
    updateBasicInfo();
}, 100);

// Post Get basic function

async function postData(url = '', data = {}) {
    const response = await queuedFetch(url, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(data)
    });
    
    const result = await response.json();
    
    // 检查响应状态和业务逻辑错误
    if (!response.ok || (result.code && result.code !== 200)) {
        const error = new Error(result.msg || `HTTP error! status: ${response.status}`);
        error.status = response.status;
        error.code = result.code;
        throw error;
    }
    
    return result;
}

async function fetchData(url = '', options = {}) {
    try {
        const response = await queuedFetch(url, options);

        if (!response.ok) {
            throw new Error(`HTTP错误! 状态码: ${response.status}`);
        }

        return await response.json();
    } catch (error) {
        // 处理不同类型的错误
        if (error.name === 'AbortError') {
            throw new Error('请求超时，请检查网络连接');
        } else if (error.name === 'TypeError' && error.message.includes('Failed to fetch')) {
            throw new Error('网络连接失败，请检查设备是否在线');
        } else {
            throw error; // 传递其他错误
        }
    }
}

// Serial config toggle

// 添加折叠功能
function initializeToggle(toggleId, contentId) {
    const toggleBtn = document.getElementById(toggleId);
    const contentArea = document.getElementById(contentId);
    let isCollapsed = false;

    toggleBtn.addEventListener('click', () => {
        isCollapsed = !isCollapsed;
        toggleBtn.classList.toggle('collapsed');

        if (isCollapsed) {
            contentArea.style.maxHeight = '0';
            contentArea.classList.add('collapsed');
        } else {
            contentArea.style.maxHeight = contentArea.scrollHeight + 'px';
            contentArea.classList.remove('collapsed');
        }
    });
}

// 页面加载时初始化折叠功能
document.addEventListener('DOMContentLoaded', () => {
    // 初始化基本信息显示（确保DOM已完全加载）
    initBasicInfoElements();
    setRuntimeStatusText('mqttconnected', 'network.notConnected', '未连接');
    setRuntimeStatusText('tcpconnected', 'network.notConnected', '未连接');
    updateBasicInfo();
    
    // 初始化工作模式的折叠功能
    initializeToggle('workModeToggle', 'workModeContent');

    // 初始化串口设置的折叠功能
    initializeToggle('serialConfigToggle', 'serialConfigContent');
});

window.addEventListener('languageChanged', () => {
    updateBasicInfo();
    refreshRuntimeStatusTexts();
});

// 在内容变化时更新最大高度
const resizeObserver = new ResizeObserver(entries => {
    entries.forEach(entry => {
        if (!entry.target.classList.contains('collapsed')) {
            entry.target.style.maxHeight = entry.target.scrollHeight + 'px';
        }
    });
});

// 观察内容区域的变化
document.querySelectorAll('.content-area').forEach(area => {
    resizeObserver.observe(area);
});


// Work mode API

function getSelectedWorkMode() {
    const checkedMode = document.querySelector('input[name="work_mode"]:checked');
    return checkedMode ? checkedMode.value : 'tcp_transparent';
}

function collectWorkModePayload() {
    return {
        work_mode: getSelectedWorkMode(),
        mode_config: {}
    };
}

function updateWorkModeDescription(modeName) {
    const protocolTestInfo = document.getElementById('protocolTestInfo');
    if (protocolTestInfo) {
        protocolTestInfo.style.display = modeName === 'protocol_test' ? 'flex' : 'none';
    }
}

function applyWorkModeInfo(responseData) {
    const modeName = responseData && responseData.work_mode
        ? responseData.work_mode
        : 'tcp_transparent';
    const radio = document.querySelector(`input[name="work_mode"][value="${modeName}"]`)
        || document.querySelector('input[name="work_mode"][value="tcp_transparent"]');
    if (radio) {
        radio.checked = true;
        radio.dispatchEvent(new Event('change'));
    }
    updateWorkModeDescription(radio ? radio.value : 'tcp_transparent');
    configCache.lastWorkMode = radio ? radio.value : 'tcp_transparent';
}

// 修改工作模式切换事件处理
document.querySelectorAll('input[name="work_mode"]').forEach(radio => {
    radio.addEventListener('change', function () {
        updateWorkModeDescription(this.value);

        const workModeContent = document.getElementById('workModeContent');
        const setWorkModeButton = document.getElementById('setWorkModeButton');
        const serialConfigContent = document.getElementById('serialConfigContent');
        const serialConfigCard = serialConfigContent ? serialConfigContent.closest('.card-view') : null;
        const tcpButtons = [
            document.getElementById('tcpServer'),
            document.getElementById('tcpClient'),
            document.getElementById('tcpModbusServer'),
            document.getElementById('tcpModbusClient')
        ];

        tcpButtons.forEach(btn => {
            if (btn) btn.disabled = false;
        });
        if (serialConfigCard) serialConfigCard.style.display = 'block';
        if (setWorkModeButton) {
            setWorkModeButton.style.display = 'block';
        }

        if (workModeContent) {
            workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
            workModeContent.classList.remove('collapsed');
        }
        const workModeToggle = document.getElementById('workModeToggle');
        if (workModeToggle) workModeToggle.classList.remove('collapsed');
    });
});

// 页面加载时初始化工作模式区域
document.addEventListener('DOMContentLoaded', () => {
    workModeFetchData();
});

// 为设置按钮添加点击事件
document.getElementById('setWorkModeButton').addEventListener('click', workModeSubmit);

async function workModeSubmit() {
    const workMode = getSelectedWorkMode();
    const data = collectWorkModePayload();

    if (workMode === 'tcp_transparent') {
        const protocolCheckResult = await checkProtocolsForTcpTransparentMode();
        if (!protocolCheckResult.valid) {
            const detailMessage = protocolCheckResult.unconfiguredProtocols && protocolCheckResult.unconfiguredProtocols.length > 0
                ? `请先在协议管理中启用：${protocolCheckResult.unconfiguredProtocols.join('、')}。`
                : '请先完成相关协议的启用与参数配置。';
            const finalMessage = protocolCheckResult.message
                ? `${protocolCheckResult.message}\n${detailMessage}`
                : detailMessage;
            showCustomAlert(finalMessage, true);
            setI18nText('setWorkModeButton', 'message.protocolDisabled', '协议未启用');
            setTimeout(() => setI18nText('setWorkModeButton', 'common.set', '设置'), 2000);
            return;
        }
    }

    try {
        console.log('提交工作模式配置:', JSON.stringify(data, null, 2));
        await postData('/work_mode_set', data);
        setI18nText('setWorkModeButton', 'message.setSuccess', '设置成功');

        const deviceRestart = document.getElementById('deviceRestart');
        if (deviceRestart) {
            showSvg('deviceRestart', 6000);
        }
    } catch (error) {
        console.error('Error saving work mode:', error);
        if (error.code === 400 && error.message) {
            showCustomAlert(error.message, true);
            setI18nText('setWorkModeButton', 'message.configCheckFailed', '配置检查失败');
        } else {
            setI18nText('setWorkModeButton', 'message.setFailed', '设置失败');
        }
    }
    setTimeout(() => setI18nText('setWorkModeButton', 'common.set', '设置'), 2000);
}

// 页面加载时获取工作模式配置
async function workModeFetchData(retryCount = 0) {
    const maxRetries = 3;
    const retryDelay = 1000;

    try {
        console.log(`正在获取工作模式配置... (尝试 ${retryCount + 1}/${maxRetries + 1})`);
        const responseData = await fetchData('/work_mode_info');
        console.log('工作模式配置:', responseData);

        if (responseData) {
            configCache.lastWorkMode = responseData.work_mode || 'tcp_transparent';
            configCache.modeConfig = responseData.mode_config || {};
            configCache.availableModes = responseData.available_modes || [];
            applyWorkModeInfo(responseData);
            return;
        }
    } catch (error) {
        console.error('Failed to fetch work mode data:', error);
    }

    if (retryCount < maxRetries) {
        console.warn(`获取工作模式配置失败，${retryDelay}ms后重试...`);
        setTimeout(() => workModeFetchData(retryCount + 1), retryDelay);
        return;
    }

    const checkedMode = document.querySelector('input[name="work_mode"]:checked');
    if (checkedMode) {
        checkedMode.dispatchEvent(new Event('change'));
    }
}

// Serial config API

document.getElementById("serialDataForm").addEventListener("submit", event => event.preventDefault());

document.getElementById('serialButton').addEventListener('click', async () => {
    const formData = new FormData(document.getElementById('serialDataForm'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/serial_set', data);
    } catch (error) {
        console.error('Error fetching data. Status:', error);
    }
});

async function serialfetchData() {
    try {
        const responseData = await fetchData('/serial_set_info');
        document.getElementById('baud_rate').value = responseData.baud_rate || '9600';
        document.getElementById('data_bit').value = responseData.data_bit || '8';

        // 根据后端返回值设置校验位
        let checkBit = 'None';  // 默认无校验
        if (responseData.check_bit === '1') {
            checkBit = 'Odd';  // 奇校验
        } else if (responseData.check_bit === '2') {
            checkBit = 'Even';  // 偶校验
        }
        document.getElementById('check_bit').value = checkBit;

        // 设置停止位
        let stopBit = '1';  // 默认1位停止位
        if (responseData.stop_bit === '1') {
            stopBit = '1';
        } else if (responseData.stop_bit === '2') {
            stopBit = '2';
        } else if (responseData.stop_bit === '1.5') {
            stopBit = '1.5';
        }
        document.getElementById('stop_bit').value = stopBit;

        document.getElementById('frame_time').value = responseData.frame_time || '50';
        document.getElementById('frame_len').value = responseData.frame_len || '512';
        console.log(responseData);
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}
serialfetchData();

// Serial control API

document.getElementById("serial_ctl").addEventListener("submit", event => event.preventDefault());

function convertInstructionToBytes(rawInstruction, isHexMode) {
    if (!rawInstruction) {
        return new Uint8Array(0);
    }

    if (isHexMode) {
        const cleanStr = rawInstruction.replace(/[^0-9A-Fa-f]/g, '');
        if (cleanStr.length === 0) {
            return new Uint8Array(0);
        }
        if (cleanStr.length % 2 !== 0) {
            throw new Error('十六进制字符数量必须为偶数');
        }
        const byteLen = cleanStr.length / 2;
        const bytes = new Uint8Array(byteLen);
        for (let i = 0; i < byteLen; i++) {
            const byte = parseInt(cleanStr.substr(i * 2, 2), 16);
            if (Number.isNaN(byte)) {
                throw new Error('检测到无效的十六进制字符');
            }
            bytes[i] = byte;
        }
        return bytes;
    }

    return textEncoder.encode(rawInstruction);
}

function bytesToHex(bytes) {
    return Array.from(bytes)
        .map((byte) => byte.toString(16).padStart(2, '0').toUpperCase())
        .join(' ');
}

function calculateCrc16Modbus(bytes) {
    let crc = 0xFFFF;

    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc & 0xFFFF;
}

function getCrcBytes(bytes) {
    const crc = calculateCrc16Modbus(bytes);
    return new Uint8Array([crc & 0xFF, (crc >> 8) & 0xFF]);
}

function isSendCrcEnabled() {
    const crcCheckbox = document.getElementById('sendCrcEnabled');
    return Boolean(crcCheckbox && crcCheckbox.checked);
}

function setCrcPreview(text, isEnabled) {
    const previewElement = document.getElementById('crcPreview');
    if (!previewElement) return;

    previewElement.textContent = text || '-- --';
    previewElement.classList.toggle('disabled', !isEnabled);
}

function updateCrcPreview() {
    const isEnabled = isSendCrcEnabled();
    const instructionValue = document.getElementById('hiddenInput')?.value || '';
    const sendType = document.getElementById('tran')?.checked ? 'hex' : 'ascii';

    if (!isEnabled) {
        setCrcPreview('-- --', false);
        return;
    }

    try {
        const payloadBytes = convertInstructionToBytes(instructionValue, sendType === 'hex');
        if (payloadBytes.length === 0) {
            setCrcPreview('-- --', true);
            return;
        }

        setCrcPreview(bytesToHex(getCrcBytes(payloadBytes)), true);
    } catch (error) {
        setCrcPreview('ERR', true);
    }
}

function appendCrcBytes(bytes) {
    if (!isSendCrcEnabled()) {
        return bytes;
    }

    const crcBytes = getCrcBytes(bytes);
    const bytesWithCrc = new Uint8Array(bytes.length + crcBytes.length);
    bytesWithCrc.set(bytes);
    bytesWithCrc.set(crcBytes, bytes.length);
    return bytesWithCrc;
}

async function sendInstructionChunks(bytes, sendType, transferId) {
    if (bytes.length === 0) {
        throw new Error('请输入要发送的数据');
    }

    await waitForUartWebSocketOpen();

    const totalChunks = Math.max(1, Math.ceil(bytes.length / UART_CHUNK_BYTES));
    for (let chunkIndex = 0; chunkIndex < totalChunks; chunkIndex++) {
        const start = chunkIndex * UART_CHUNK_BYTES;
        const end = Math.min(start + UART_CHUNK_BYTES, bytes.length);
        const chunk = bytes.slice(start, end);
        if (!chunk.length) {
            continue;
        }

        const payload =
            sendType === 'hex'
                ? bytesToHex(chunk)
                : textDecoder.decode(chunk);

        const message = {
            type: 'uart_tx',
            instruction: payload,
            sendType,
            chunkIndex,
            chunkTotal: totalChunks,
            transferId
        };

        const ackPromise = createAckPromise(transferId, chunkIndex);
        try {
            // 修复：使用统一的WebSocket连接（ws），而不是废弃的uartWebSocket
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                throw new Error('WebSocket未连接，请稍后重试');
            }
            ws.send(JSON.stringify(message));
        } catch (error) {
            const key = buildAckKey(transferId, chunkIndex);
            const pending = pendingUartAcks.get(key);
            if (pending) {
                pending.reject(error.message || '发送失败');
            }
            throw new Error('串口发送失败: ' + (error.message || '未知错误'));
        }

        await ackPromise;
    }
}

async function pollLegacyUartResponse() {
    let attempts = 0;
    const maxAttempts = 10;
    const pollInterval = 50;

    while (attempts < maxAttempts) {
        try {
            const response = await fetch('/uart_response');
            const responseData = await response.json();
            if (responseData.tx_hex || responseData.rx_hex || responseData.tx_ascii || responseData.rx_ascii) {
                const resultElement = document.getElementById('devcie_report');

                if (responseData.tx_hex || responseData.tx_ascii) {
                    const txTimestamp = formatTimestamp(responseData.tx_timestamp);
                    if (document.getElementById('tran').checked) {
                        resultElement.innerHTML += `[${txTimestamp}]发→◇${responseData.tx_hex} <br>`;
                    } else {
                        resultElement.innerHTML += `[${txTimestamp}]发→◇${responseData.tx_ascii} <br>`;
                    }
                }

                if (responseData.rx_hex || responseData.rx_ascii) {
                    const rxTimestamp = formatTimestamp(responseData.rx_timestamp, true);
                    if (document.getElementById('tran').checked) {
                        resultElement.innerHTML += `[${rxTimestamp}]收←◆${responseData.rx_hex}<br>`;
                    } else {
                        resultElement.innerHTML += `[${rxTimestamp}]收←◆${responseData.rx_ascii}<br>`;
                    }
                }

                resultElement.scrollTop = resultElement.scrollHeight;
                return;
            }
        } catch (error) {
            console.error('Error polling response:', error);
            return;
        }

        attempts++;
        await new Promise(resolve => setTimeout(resolve, pollInterval));
    }
}

// 修改 serialSubmit 函数
async function serialSubmit() {
    const instructionValue = document.getElementById('hiddenInput').value;
    const sendType = document.getElementById('tran').checked ? 'hex' : 'ascii';
    const sendButton = document.getElementById('serialControl');

    try {
        const payloadBytes = convertInstructionToBytes(instructionValue, sendType === 'hex');
        if (payloadBytes.length === 0) {
            showCustomAlert('请输入要发送的数据', true);
            return;
        }
        const sendBytes = appendCrcBytes(payloadBytes);

        const transferId = `${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;
        if (sendButton) {
            sendButton.disabled = true;
        }
        const needProgressAlert = sendBytes.length > UART_CHUNK_BYTES;
        if (needProgressAlert) {
            showCustomAlert(`开始发送串口数据，共${sendBytes.length}字节`, false);
        }

        await sendInstructionChunks(sendBytes, sendType, transferId);
        await pollLegacyUartResponse(); // 兼容老版本

        if (needProgressAlert) {
            showCustomAlert(`串口数据发送完成，共${sendBytes.length}字节`, false);
        }
    } catch (error) {
        console.error('Error fetching data:', error);
        showCustomAlert(error.message || '串口数据发送失败', true);
    } finally {
        if (sendButton) {
            sendButton.disabled = false;
        }
    }
}

document.getElementById('serialControl').addEventListener('click', async event => {
    event.preventDefault();
    const instructionElement = document.getElementById('instruction');
    const instruction = instructionElement.innerText;
    const errorMessageElement = document.getElementById('error-message');
    const isHex = /^[0-9A-Fa-f\s]+$/i.test(instruction.replace(/\s/g, ''));
    const tranElement = document.getElementById('tran');

    if (tranElement.checked && !isHex) {
        errorMessageElement.innerText = '数据格式异常，请输入十六进制数据';
        showCustomAlert('数据格式异常，请输入十六进制数据', true);
        event.preventDefault();
    } else {
        errorMessageElement.innerText = '';
        serialSubmit();
    }
});

// 添加清空按钮事件监听
document.getElementById('clearResponse').addEventListener('click', () => {
    const responseElement = document.getElementById('devcie_report');
    responseElement.innerHTML = '';
});

document.getElementById('clearInstruction').addEventListener('click', () => {
    const instructionElement = document.getElementById('instruction');
    instructionElement.innerText = '';
    document.getElementById('hiddenInput').value = '';
    document.getElementById('error-message').innerText = '';
    updateCrcPreview();
});

const instructionInput = document.getElementById('instruction');
if (instructionInput) {
    instructionInput.addEventListener('input', () => {
        const hiddenInput = document.getElementById('hiddenInput');
        if (hiddenInput) {
            hiddenInput.value = instructionInput.innerText;
        }
        updateCrcPreview();
    });
}

['tran', 'mqt', 'sendCrcEnabled'].forEach(id => {
    const element = document.getElementById(id);
    if (element) {
        element.addEventListener('change', updateCrcPreview);
    }
});
// 添加接收状态控制变量
const MAX_LINES = 512; // 最多显示行
const UART_CHUNK_BYTES = 512; // 每段发送的最大字节数
let isReceiving = true;
let pollInterval = null;
// 注释：uartWebSocket已废弃，统一使用ws连接（在第3896行定义）
const pendingUartAcks = new Map();
const uartChunkBuffer = new Map();
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

updateCrcPreview();

function buildAckKey(transferId, chunkIndex) {
    const id = transferId && transferId.length ? transferId : '__single__';
    const idx = Number.isFinite(chunkIndex) ? chunkIndex : 0;
    return `${id}:${idx}`;
}

function rejectAllPendingAcks(message) {
    const reason = message || 'WebSocket连接已中断';
    pendingUartAcks.forEach(handler => {
        if (handler && typeof handler.reject === 'function') {
            handler.reject(reason);
        }
    });
    pendingUartAcks.clear();
    uartChunkBuffer.clear();
}

function createAckPromise(transferId, chunkIndex, timeoutMs = 5000) {
    const key = buildAckKey(transferId, chunkIndex);
    if (pendingUartAcks.has(key)) {
        pendingUartAcks.get(key).reject('存在重复的发送请求');
    }

    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            pendingUartAcks.delete(key);
            reject(new Error('串口发送超时'));
        }, timeoutMs);

        pendingUartAcks.set(key, {
            resolve: () => {
                clearTimeout(timer);
                pendingUartAcks.delete(key);
                resolve();
            },
            reject: (message) => {
                clearTimeout(timer);
                pendingUartAcks.delete(key);
                reject(message instanceof Error ? message : new Error(message || '串口发送失败'));
            }
        });
    });
}

async function waitForUartWebSocketOpen(timeoutMs = 5000) {
    // 修复：使用统一的WebSocket连接（ws），而不是废弃的uartWebSocket
    if (ws && ws.readyState === WebSocket.OPEN) {
        return;
    }

    if (!ws || ws.readyState === WebSocket.CLOSED) {
        initWebSocket();
    }

    const start = Date.now();
    while (true) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            return;
        }
        if (Date.now() - start > timeoutMs) {
            throw new Error('串口WebSocket连接失败');
        }
        await new Promise(resolve => setTimeout(resolve, 100));
    }
}
function appendUartResult(timestamp, isTx, hexPayload, asciiPayload) {
    const resultElement = document.getElementById('devcie_report');
    if (!resultElement) return;

    const formattedTime = formatTimestamp(timestamp, !isTx);
    const direction = isTx ? '发→◇' : '收←◆';
    const showHex = document.getElementById('tran').checked;
    const content = showHex ? (hexPayload || '').trim() : (asciiPayload || '');

    resultElement.innerHTML += `[${formattedTime}]${direction}${content}<br>`;

    const lines = resultElement.innerHTML.split('<br>');
    if (lines.length > MAX_LINES) {
        resultElement.innerHTML = lines.slice(-MAX_LINES).join('<br>');
    }
    resultElement.scrollTop = resultElement.scrollHeight;
}

function getFrameKey(data) {
    const frameId = data.frameId || data.timestamp || `${Date.now()}`;
    const direction = data.is_tx ? 'tx' : 'rx';
    return `${frameId}-${direction}`;
}

function handleUartDataMessage(data) {
    if (!isReceiving) return;
    const chunkTotal = data.chunkTotal || data.chunk_total || 1;
    const chunkIndex = data.chunkIndex || data.chunk_index || 0;

    if (chunkTotal <= 1) {
        appendUartResult(data.timestamp, data.is_tx, data.hex, data.ascii);
        return;
    }

    const key = getFrameKey(data);
    let entry = uartChunkBuffer.get(key);
    if (!entry) {
        entry = {
            timestamp: data.timestamp,
            isTx: data.is_tx,
            chunkTotal,
            hexChunks: new Array(chunkTotal).fill(''),
            asciiChunks: new Array(chunkTotal).fill(''),
            received: 0,
            receivedSet: new Set()
        };
        uartChunkBuffer.set(key, entry);
    }

    if (!entry.receivedSet.has(chunkIndex)) {
        entry.receivedSet.add(chunkIndex);
        entry.received++;
    }
    entry.hexChunks[chunkIndex] = data.hex || '';
    entry.asciiChunks[chunkIndex] = data.ascii || '';

    if (entry.received >= entry.chunkTotal) {
        uartChunkBuffer.delete(key);
        const fullHex = entry.hexChunks.join('').trim();
        const fullAscii = entry.asciiChunks.join('');
        appendUartResult(entry.timestamp, entry.isTx, fullHex, fullAscii);
    }
}

// 获取当前时间的小时:分钟部分
function getCurrentTimePrefix() {
    const now = new Date();
    const hours = String(now.getHours()).padStart(2, '0');
    const minutes = String(now.getMinutes()).padStart(2, '0');
    return `${hours}:${minutes}`;
}

// 格式化时间戳，只处理毫秒部分，并减去100ms的延迟
function formatTimestamp(timestamp, isRx = false) {
    if (isRx) {
        timestamp = Math.max(0, timestamp - 100);
    }

    const seconds = Math.floor(timestamp / 1000000);
    const milliseconds = Math.floor((timestamp % 1000000) / 1000);

    return `${getCurrentTimePrefix()}:${String(seconds % 60).padStart(2, '0')}.${String(milliseconds).padStart(3, '0')}`;
}

// 初始化串口WebSocket连接
function initUartWebSocket() {
    // 已经废弃：与initWebSocket()合并，避免重复连接
    // 此函数保留空壳，防止调用报错
    console.log('initUartWebSocket() 已废弃，使用统一的WebSocket连接');
}

// 修改轮询函数，保留以兼容旧版本，但不再主动调用
async function pollUartData() {
    // 此函数保留但不再使用，改为WebSocket方式接收数据
    console.log('pollUartData函数已弃用，改为使用WebSocket接收数据');
}

document.getElementById('toggleReceive').addEventListener('click', function () {
    const button = this;
    isReceiving = !isReceiving;

    if (isReceiving) {
        setI18nText(button, 'serialConfig.stopReceive', '停止接收');
    } else {
        setI18nText(button, 'serialConfig.resumeReceive', '继续接收');
    }
});

// 添加清空按钮事件监听
document.getElementById('clearResponse').addEventListener('click', function () {
    const resultElement = document.getElementById('devcie_report');
    resultElement.innerHTML = ''; // 清空内容
});

// 修改页面加载时的初始化
document.addEventListener('DOMContentLoaded', () => {
    // 初始化串口WebSocket连接
    initUartWebSocket();

    // 不再使用轮询
    // pollInterval = setInterval(pollUartData, 50);
});

// 页面不可见时的处理
document.addEventListener('visibilitychange', () => {
    // 不再需要处理轮询间隔
});

// Net config API

window.onload = () => autoDHCP(document.getElementById('is_dhcp'));

function autoDHCP(element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    document.getElementById('is_dhcp').value = element.classList.contains('on') ? '2' : '1';
    ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => {
        if (element.classList.contains('on')) {
            document.getElementById(id).removeAttribute('disabled');
        } else {
            document.getElementById(id).setAttribute('disabled', true);
        }
    });
}

function changeNetSelect(id) {
    [...document.getElementsByClassName('network-btn')].forEach(btn => btn.style.background = '#898989a1');
    document.getElementById(id).style.background = 'var(--BRAND)';
    document.getElementById('selectedNetwork').value = (id === 'ethernet') ? 1 : 2;

    // 保持静态IP设置不变，而不是重置它
    // 获取当前静态IP设置状态
    const currentDHCPValue = document.getElementById('is_dhcp').value;

    // 显示或隐藏WiFi相关字段
    ['wifi_ssid_div', 'wifi_password_div', 'wifisearch', 'scanList'].forEach(el =>
        document.getElementById(el).style.display = (id === 'ethernet') ? 'none' : 'flex');
}

document.getElementById("net_set").addEventListener("submit", event => event.preventDefault());

function validateIP(ip) {
    const ipPattern = /^(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$/;
    return ipPattern.test(ip);
}

async function netsetSubmit() {
    let isValid = true;
    const fields = [];
    const selectedNetwork = document.getElementById('selectedNetwork').value;
    const isDHCP = document.getElementById('is_dhcp').value;
    if ((selectedNetwork == '1' && isDHCP == '2') || (selectedNetwork == '2' && isDHCP == '2')) {
        fields.push({
            id: 'static_ip',
            errMsg: 'IP地址格式不正确',
            validate: validateIP
        }, {
            id: 'static_netmask',
            errMsg: '子网掩码格式不正确',
            validate: validateIP
        },
            {
                id: 'static_gateway',
                errMsg: '网关格式不正确',
                validate: validateIP
            },
            {
                id: 'static_dns1',
                errMsg: 'DNS1格式不正确',
                validate: validateIP
            },
            {
                id: 'static_dns2',
                errMsg: 'DNS2格式不正确',
                validate: validateIP,
                optional: true // DNS2是可选的
            });
    }
    if (selectedNetwork == '2') {
        const ssid = document.getElementById('wifi_ssid').value;
        const password = document.getElementById('wifi_password').value;
        if (!ssid) {
            // 修正获取WiFi名称错误信息容器的方式
            const ssidInput = document.getElementById('wifi_ssid');
            const ssidErrorContainer = ssidInput.parentElement.querySelector('.err-text');
            if (ssidErrorContainer) {
                ssidErrorContainer.textContent = tr('message.wifiSsidRequired', 'WiFi名称不能为空');
                ssidErrorContainer.style.color = 'red';
                ssidErrorContainer.style.display = 'block';
                ssidInput.style.borderColor = 'red';
            }
            isValid = false;
        } else {
            // 清除错误提示
            const ssidInput = document.getElementById('wifi_ssid');
            const ssidErrorContainer = ssidInput.parentElement.querySelector('.err-text');
            if (ssidErrorContainer) {
                ssidErrorContainer.textContent = '';
                ssidErrorContainer.style.display = 'none';
            }
            ssidInput.style.borderColor = '';
        }

        if (!password || password.length < 8) {
            // 修正获取WiFi密码错误信息容器的方式
            // 找到WiFi密码输入框的父级元素下的err-text元素
            const passwordInput = document.getElementById('wifi_password');
            const errorContainer = passwordInput.parentElement.querySelector('.err-text');
            if (errorContainer) {
                errorContainer.textContent = tr('message.wifiPasswordInvalid', 'WiFi密码不能为空且最少8位');
                // 添加样式让错误信息更明显
                errorContainer.style.color = 'red';
                errorContainer.style.display = 'block';
                // 让密码输入框有错误提示样式
                passwordInput.style.borderColor = 'red';

                // 调整眼睛图标位置
                const eyeOpen = document.getElementById('wifieyeOpen');
                const eyeClosed = document.getElementById('wifieyeClosed');
                if (eyeOpen) eyeOpen.style.top = '15px';
                if (eyeClosed) eyeClosed.style.top = '15px';
            }
            isValid = false;
        } else {
            // 清除错误提示
            const passwordInput = document.getElementById('wifi_password');
            const errorContainer = passwordInput.parentElement.querySelector('.err-text');
            if (errorContainer) {
                errorContainer.textContent = '';
                errorContainer.style.display = 'none';
            }
            passwordInput.style.borderColor = '';

            // 恢复眼睛图标位置
            const eyeOpen = document.getElementById('wifieyeOpen');
            const eyeClosed = document.getElementById('wifieyeClosed');
            if (eyeOpen) eyeOpen.style.top = '';
            if (eyeClosed) eyeClosed.style.top = '';
        }
    }

    for (const field of fields) {
        const input = document.getElementById(field.id);
        const errText = input.nextElementSibling;
        const value = input.value.trim();

        if (!value) {
            errText.textContent = field.errMsg;
            isValid = false;
        } else if (field.validate && !field.validate(value)) {
            errText.textContent = field.errMsg;
            isValid = false;
        } else {
            errText.textContent = '';
        }
    }

    if (!isValid) {
        return; // 如果有错误，不进行提交
    }

    const formData = new FormData(document.getElementById('net_set'));
    const data = Object.fromEntries(formData);

    // 防止切换网络模式时丢失静态IP设置
    // 如果当前页面上的静态IP开关是开启的，但表单中的is_dhcp值不匹配，修正它
    const staticIPSwitchIsOn = document.getElementById('static_ip_switch').classList.contains('on');
    if (staticIPSwitchIsOn && data.is_dhcp !== '2') {
        data.is_dhcp = '2';
    } else if (!staticIPSwitchIsOn && data.is_dhcp !== '1') {
        data.is_dhcp = '1';
    }

    try {
        await postData('/net_set', data);
        setI18nText('netSetButton', 'message.setSuccess', '设置成功');
        if (!hasRestarted) {  // 检查是否已经发送了重启命令
            const deviceRestart = document.getElementById('deviceRestart');
            if (deviceRestart) {
                showSvg('deviceRestart', 6000);
                hasRestarted = true;  // 标记重启命令已经发送
            }
        }
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        setI18nText('netSetButton', 'message.setFailed', '设置失败');
    }
    setTimeout(() => setI18nText('netSetButton', 'common.submit', '提交'), 2000);
}

document.getElementById('netSetButton').addEventListener('click', netsetSubmit);

// 为WIFI名称和密码输入框添加回车键事件监听，阻止表单提交
document.getElementById('wifi_ssid').addEventListener('keydown', function(event) {
    if (event.key === 'Enter') {
        event.preventDefault();
        event.stopPropagation();
    }
});

document.getElementById('wifi_password').addEventListener('keydown', function(event) {
    if (event.key === 'Enter') {
        event.preventDefault();
        event.stopPropagation();
    }
});

async function netsetfetchData() {
    try {
        const responseData = await fetchData('/net_set_info');
        if (!responseData) {
            console.error('No data returned from /net_set_info');
            return;
        }
        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => document.getElementById(id).value = responseData[id]);
        ['wifi_ssid', 'wifi_password'].forEach(id => {
            if (responseData[id] !== undefined) {
                document.getElementById(id).value = responseData[id] || '';
            }
        });
        if (responseData.netconn == 1 || responseData.netconn == 0) {
            document.getElementById('ethernet').click();
        }
        if (responseData.netconn == 2) {
            document.getElementById('wifi').click();
        }

        // 确保以正确的方式处理静态IP开关
        const static_ip_switch = document.getElementById('static_ip_switch');
        const isDHCP = responseData.is_dhcp || '1'; // 默认为动态IP(1)，如果未设置
        document.getElementById('is_dhcp').value = isDHCP; // 确保隐藏字段与实际值同步

        static_ip_switch.classList.remove('off', 'on');
        static_ip_switch.classList.add(isDHCP == '2' ? 'on' : 'off');

        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => {
            if (isDHCP == '1') {
                document.getElementById(id).setAttribute('disabled', true);
            } else {
                document.getElementById(id).removeAttribute('disabled');
            }
        });
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}

netsetfetchData();



// Search wifi API


function clickwifi(element) {
    // 清除WiFi相关的错误提示
    clearWifiErrors();

    // 获取当前选中的SSID
    const currentSSID = document.getElementById('wifi_ssid').value;
    // 获取新选中的SSID
    const newSSID = element.value;

    // 更新SSID输入框
    document.getElementById('wifi_ssid').value = newSSID;

    // 如果选择了不同的SSID，清空密码输入框
    if (currentSSID !== newSSID) {
        document.getElementById('wifi_password').value = '';
    }

    // 设置选中行的背景色
    [...document.querySelectorAll('#wifitable .addwifi')].forEach(row => row.style.backgroundColor = '');
    element.parentNode.parentNode.parentNode.style.backgroundColor = '#004ba93d';
}

// 添加清除WiFi相关错误的辅助函数
function clearWifiErrors() {
    // 清除SSID错误
    const ssidInput = document.getElementById('wifi_ssid');
    if (ssidInput) {
        ssidInput.style.borderColor = '';
        const ssidErrorContainer = ssidInput.parentElement.querySelector('.err-text');
        if (ssidErrorContainer) {
            ssidErrorContainer.textContent = '';
            ssidErrorContainer.style.display = 'none';
        }
    }

    // 清除密码错误
    const passwordInput = document.getElementById('wifi_password');
    if (passwordInput) {
        passwordInput.style.borderColor = '';
        const passwordErrorContainer = passwordInput.parentElement.querySelector('.err-text');
        if (passwordErrorContainer) {
            passwordErrorContainer.textContent = '';
            passwordErrorContainer.style.display = 'none';
        }

        // 恢复眼睛图标位置
        const eyeOpen = document.getElementById('wifieyeOpen');
        const eyeClosed = document.getElementById('wifieyeClosed');
        if (eyeOpen) eyeOpen.style.top = '';
        if (eyeClosed) eyeClosed.style.top = '';
    }
}

document.getElementById('wifisearch').addEventListener('click', async () => {
    const wifiSearchBtn = document.getElementById('wifisearch');

    // 如果按钮已经禁用，直接返回
    if (wifiSearchBtn.disabled) {
        return;
    }

    // 清除WiFi相关的任何错误提示
    clearWifiErrors();

    setI18nText(wifiSearchBtn, 'message.searching', '搜索中');
    wifiSearchBtn.disabled = true;

    // 移除任何现有的消息，但保留WiFi列表
    const existingMessages = document.querySelectorAll('.error-message, .info-message');
    existingMessages.forEach(msg => {
        if (msg.parentNode) msg.parentNode.removeChild(msg);
    });

    try {
        const response = await fetch('/find_wifi');

        // 检查响应状态
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const data = await response.json();

        // 处理错误情况
        if (data.error) {
            // 显示错误信息
            const errorMessage = document.createElement('div');
            errorMessage.className = 'error-message';
            errorMessage.textContent = data.error;
            document.getElementById('scanList').parentNode.insertBefore(errorMessage, document.getElementById('scanList'));

            // 自动移除错误信息
            setTimeout(() => {
                if (errorMessage && errorMessage.parentNode) {
                    errorMessage.parentNode.removeChild(errorMessage);
                }
            }, 5000);

            // 延迟一段时间后允许再次点击
            const waitTime = data.wait_time || 5000;

            // 显示倒计时
            let remainingTime = Math.ceil(waitTime / 1000);
            setI18nFormatText(wifiSearchBtn, 'message.waitSeconds',
                { seconds: remainingTime }, `请等待 ${remainingTime}秒`);

            const countdownInterval = setInterval(() => {
                remainingTime--;
                if (remainingTime <= 0) {
                    clearInterval(countdownInterval);
                    setI18nText(wifiSearchBtn, 'network.search', '搜索');
                    wifiSearchBtn.disabled = false;
                } else {
                    setI18nFormatText(wifiSearchBtn, 'message.waitSeconds',
                        { seconds: remainingTime }, `请等待 ${remainingTime}秒`);
                }
            }, 1000);

            return; // 保留现有WiFi列表
        }

        // 处理未找到WiFi的情况
        if (data.message) {
            const messageElement = document.createElement('div');
            messageElement.className = 'info-message';
            messageElement.textContent = data.message;
            document.getElementById('scanList').parentNode.insertBefore(messageElement, document.getElementById('scanList'));

            setTimeout(() => {
                if (messageElement && messageElement.parentNode) {
                    messageElement.parentNode.removeChild(messageElement);
                }
            }, 5000);

            setTimeout(() => {
                setI18nText(wifiSearchBtn, 'network.search', '搜索');
                wifiSearchBtn.disabled = false;
            }, 1000);

            return; // 保留现有WiFi列表
        }

        // 只有在成功获取到新的WiFi列表时才清空并更新
        // 确保数据不为空
        if (Object.keys(data).length > 0) {
            document.getElementById('scanList').style.display = 'flex';
            [...document.getElementsByClassName('addwifi')].forEach(tr => tr.remove());

            // 移除任何可能存在的错误或信息消息
            const messages = document.querySelectorAll('.error-message, .info-message');
            messages.forEach(msg => msg.parentNode && msg.parentNode.removeChild(msg));

            // 转换成数组以便排序
            const wifiList = Object.entries(data).map(([ssid, rssi]) => ({ ssid, rssi }));

            // 按信号强度从强到弱排序
            wifiList.sort((a, b) => b.rssi - a.rssi);

            // 显示排序后的结果
            for (const wifi of wifiList) {
                const tr = document.createElement("tr");
                tr.setAttribute('class', 'addwifi');
                let td = document.createElement("td");
                td.innerHTML = `<label class='bui-radios-label'><input type='radio' name='wifi'  onclick='clickwifi(this)' value='${wifi.ssid}'/><i class='bui-radios'></i> ${wifi.ssid}</label>`;
                tr.appendChild(td);
                td = document.createElement("td");
                td.innerHTML = `${wifi.rssi}dBm `;
                tr.appendChild(td);
                document.getElementById('clusss').parentNode.insertBefore(tr, document.getElementById('clusss'));
            }
        } else {
            // 数据为空但不是错误情况，显示提示信息
            const messageElement = document.createElement('div');
            messageElement.className = 'info-message';
            messageElement.textContent = tr('message.noWifiFound', '未找到任何WiFi网络');
            document.getElementById('scanList').parentNode.insertBefore(messageElement, document.getElementById('scanList'));

            setTimeout(() => {
                if (messageElement && messageElement.parentNode) {
                    messageElement.parentNode.removeChild(messageElement);
                }
            }, 5000);
        }

        setI18nText(wifiSearchBtn, 'network.search', '搜索');
        wifiSearchBtn.disabled = false;
    } catch (error) {
        console.error('Error fetching data:', error);
        const errorMessage = document.createElement('div');
        errorMessage.className = 'error-message';
        errorMessage.textContent = tr('message.networkErrorRetry', '网络错误，请稍后重试');
        document.getElementById('scanList').parentNode.insertBefore(errorMessage, document.getElementById('scanList'));

        setTimeout(() => {
            if (errorMessage && errorMessage.parentNode) {
                errorMessage.parentNode.removeChild(errorMessage);
            }
        }, 3000);

        setTimeout(() => {
            setI18nText(wifiSearchBtn, 'network.search', '搜索');
            wifiSearchBtn.disabled = false;
        }, 1000);
    }
});

// MQTT config API

const useMqttStates = document.getElementById('useMqttState');
const mqttInputs = document.querySelectorAll('.mqtt-input');
const mqttButtons = document.querySelectorAll('.mqtt-btn');

function toggleInputs() {
    const isDisabled = useMqttState.value == '0';
    mqttInputs.forEach(input => input.disabled = isDisabled);
    mqttButtons.forEach(button => button.disabled = isDisabled);
}

toggleInputs();
useMqttStates.addEventListener('change', toggleInputs);

function toggleMqttSwitch(element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    document.getElementById('useMqttState').value = element.classList.contains('on') ? '1' : '0';
    toggleInputs();
}

function toggleMqttRetain(element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    document.getElementById('useMqttRetain').value = element.classList.contains('on') ? '1' : '0';
    toggleInputs();
}

function changeQosSelect(id) {
    var buttons = document.getElementsByClassName('qos-btn');
    Array.from(buttons).forEach(button => button.style.background = '#898989a1');
    document.getElementById('qos' + id).style.background = 'var(--BRAND)';
    document.getElementById('selectedQos').value = id;
}

document.getElementById("mqttDataForm").addEventListener("submit", event => event.preventDefault());

function isValidIPorDomain(value) {
    const ipPattern = /^(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$/;
    const domainPattern = /^(?!-)(?:[a-zA-Z0-9-]{0,62}[a-zA-Z0-9]\.)+[a-zA-Z]{2,}$/;
    return ipPattern.test(value) || domainPattern.test(value);
}
function validateIntervalTime(value) {
    const intervalTime = parseInt(value, 10);
    return !isNaN(intervalTime) && intervalTime > 2;
}

async function mqttSubmit() {
    const fields = [
        { id: 'mqtt_server', errMsg: '请输入服务器地址' },
        { id: 'mqtt_port', errMsg: '请输入服务器端口', isNumeric: true },
        { id: 'mqtt_username', errMsg: '请输入用户名' },
        { id: 'mqtt_clientid', errMsg: '请输入ClientID' },
        { id: 'mqtt_sub_topic', errMsg: '请输入订阅主题' },
        { id: 'mqtt_pub_topic', errMsg: '请输入发布主题' }
    ];

    for (const field of fields) {
        const input = document.getElementById(field.id);
        const errText = input.nextElementSibling;
        if (!input.value.trim()) {
            errText.textContent = field.errMsg;
            return;
        } else if (field.isNumeric && isNaN(input.value)) {
            errText.textContent = tr('message.numberRequired', '请输入数字');
            return;
        } else {
            errText.textContent = '';
        }
    }

    const formData = new FormData(document.getElementById('mqttDataForm'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/mqtt', data);
        setI18nText('mqttSubmitButton', 'message.saveSuccess', '保存成功');
        const deviceRestart = document.getElementById('deviceRestart');
        if (deviceRestart) {
            showSvg('deviceRestart', 6000);
        }
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        setI18nText('mqttSubmitButton', 'message.saveFailed', '保存失败');
    }
    setTimeout(() => setI18nText('mqttSubmitButton', 'common.submit', '提交'), 2000);
}

document.getElementById('mqttSubmitButton').addEventListener('click', mqttSubmit);

function renderMqttRuntimeStatus(responseData) {
    const mqttconnected = document.getElementById('mqttconnected');
    if (!mqttconnected || !responseData) return;

    const isMqttConnected = responseData.mqttconn == "1" || responseData.mqttconn === 1;
    const isMqttEnabled = responseData.use_mqtt == "1" || responseData.use_mqtt === 1;

    setRuntimeStatusText(
        mqttconnected,
        isMqttConnected && isMqttEnabled ? 'common.connected' : 'network.notConnected',
        isMqttConnected && isMqttEnabled ? '已连接' : '未连接'
    );
    mqttconnected.style.color = isMqttConnected && isMqttEnabled ? "#5cb85c" : "#d9534f";
}

async function mqttfetchData() {
    const maxRetries = 3;
    let retryCount = 0;
    let success = false;

    while (retryCount < maxRetries && !success) {
        try {
            const responseData = await fetchData('/mqtt_info');
            success = true;

            const mqttInputs = document.querySelectorAll('.mqtt-input');
            const mqttButtons = document.querySelectorAll('.mqtt-btn');
            const mqttconnected = document.getElementById('mqttconnected');

            if (mqttconnected && responseData) {
                document.getElementById('use_mqtt').className = responseData.use_mqtt == 1 ? "switch-btn switch-btn on" : "switch-btn switch-btn off";
                mqttInputs.forEach(input => input.disabled = responseData.use_mqtt == 0);
                mqttButtons.forEach(button => button.disabled = responseData.use_mqtt == 0);

                if (responseData.qos !== undefined && document.getElementById(`qos${responseData.qos}`)) {
                    document.getElementById(`qos${responseData.qos}`).click();
                }

                ['mqtt_server', 'mqtt_port', 'mqtt_username', 'mqtt_password', 'mqtt_clientid', 'mqtt_sub_topic', 'mqtt_pub_topic'].forEach(id => {
                    const element = document.getElementById(id);
                    if (element && responseData[id] !== undefined) {
                        element.value = responseData[id];
                    }
                });

                const useMqttState = document.getElementById('useMqttState');
                if (useMqttState) useMqttState.value = responseData.use_mqtt;

                const useMqttType = document.getElementById('useMqttType');
                if (useMqttType) useMqttType.value = responseData.mqtt_type;

                const useMqttRetain = document.getElementById('useMqttRetain');
                if (useMqttRetain) useMqttRetain.value = responseData.retain;

                const selectRetain = document.getElementById('selectRetain');
                if (selectRetain) selectRetain.className = responseData.retain == 1 ? "switch-btn switch-btn on" : "switch-btn switch-btn off";

                // 添加调试信息
                console.log('MQTT Status Debug:', {
                    mqttconn: responseData.mqttconn,
                    mqttconn_type: typeof responseData.mqttconn,
                    use_mqtt: responseData.use_mqtt,
                    use_mqtt_type: typeof responseData.use_mqtt
                });

                renderMqttRuntimeStatus(responseData);
            }
        } catch (error) {
            console.error(`尝试 ${retryCount + 1}/${maxRetries} 失败，原因:`, error);
            retryCount++;

            // 如果不是最后一次重试，则等待一段时间后再重试
            if (retryCount < maxRetries) {
                await new Promise(resolve => setTimeout(resolve, 1000 * retryCount)); // 递增等待时间
            } else {
                console.error('MQTT数据获取失败，已达到最大重试次数。最终错误:', error);

                // 在UI上显示错误信息
                const mqttconnected = document.getElementById('mqttconnected');
                if (mqttconnected) {
                    setRuntimeStatusText(mqttconnected, 'message.fetchFailed', '获取失败');
                    mqttconnected.style.color = "#d9534f";
                }

                // 可以选择显示一个用户友好的错误提示
                showCustomAlert("MQTT配置信息获取失败，请检查网络连接或设备状态。", true);
            }
        }
    }
}

// 确保在页面加载时执行
document.addEventListener('DOMContentLoaded', () => {
    // 其他现有的DOMContentLoaded事件处理程序

    // 延迟执行mqttfetchData以确保DOM已完全加载
    setTimeout(mqttfetchData, 500);
});

// 如果已经直接调用了mqttfetchData，可以删除或注释下面这行
// mqttfetchData();


// TCP config API

function renderTcpRuntimeStatus(responseData) {
    const tcpConnected = document.getElementById('tcpconnected');
    if (!tcpConnected || !responseData) return;

    const tcpMode = responseData.tcpconn || '0';
    if (tcpMode === '1' || tcpMode === '3') {
        setRuntimeStatusText(
            tcpConnected,
            responseData.tcp_server_conn == 1 ? 'common.connected' : 'network.notConnected',
            responseData.tcp_server_conn == 1 ? '已连接' : '未连接'
        );
        tcpConnected.style.color = responseData.tcp_server_conn == 1 ? "#5cb85c" : "#d9534f";
        return;
    }

    if (responseData.tcp_client_count > 0) {
        setRuntimeStatusText(
            tcpConnected,
            'tcp.clientCount',
            `当前有${responseData.tcp_client_count}个客户端连接`,
            { count: responseData.tcp_client_count }
        );
    } else {
        setRuntimeStatusText(tcpConnected, 'tcp.noDeviceConnected', '没有设备连接');
    }
    tcpConnected.style.color = responseData.tcp_client_count > 0 ? "#5cb85c" : "#d9534f";
}

    document.addEventListener('DOMContentLoaded', () => {
        const useTCPState = document.getElementById('useTCPState');
        const tcpInputs = document.querySelectorAll('.tcp-input');
        const tcpButtons = document.querySelectorAll('.tcp-btn');
        const tcpSubmitButton = document.getElementById('tcpSubmitButton');
        const tcpDataForm = document.getElementById('tcpDataForm');
        const packetFieldConfigs = [
            { selectId: 'regFormat', inputId: 'reg_packet' },
            { selectId: 'heartFormat', inputId: 'heart_packet' },
        ];

        function updatePacketInputState(selectEl, inputEl) {
            const tcpDisabled = useTCPState.value === '0';
            const disabledByFormat = selectEl.value === 'none';
            const shouldDisable = tcpDisabled || disabledByFormat;
            inputEl.disabled = shouldDisable;
            if (disabledByFormat && !tcpDisabled) {
                inputEl.classList.add('tcp-input-disabled');
            } else {
                inputEl.classList.remove('tcp-input-disabled');
            }
        }

        function applyAllPacketFieldStates() {
            packetFieldConfigs.forEach(({ selectId, inputId }) => {
                const selectEl = document.getElementById(selectId);
                const inputEl = document.getElementById(inputId);
                if (selectEl && inputEl) {
                    updatePacketInputState(selectEl, inputEl);
                }
            });
        }

        function setTcpModeUI(mode, options = {}) {
            const { preservePortValue = false } = options;
            const normalizedMode = ['0', '1'].includes(mode) ? mode : '1';
            const tcpServerBtn = document.getElementById('tcpServer');
            const tcpClientBtn = document.getElementById('tcpClient');
            const tcpServerAddress = document.getElementById('tcpServerAddress');
            const tcpRegisterPacket = document.getElementById('tcpRegisterPacket');
            const tcpHeartPacket = document.getElementById('tcpHeartPacket');
            const tcpHeartInterval = document.getElementById('tcpHeartInterval');
            const tcpPortInput = document.getElementById('tcp_port');
            const selectedTCPInput = document.getElementById('selectedTCP');

            if (selectedTCPInput) {
                selectedTCPInput.value = normalizedMode;
            }

            const buttons = [
                { btn: tcpServerBtn, id: '0' },
                { btn: tcpClientBtn, id: '1' },
            ];
            buttons.forEach(({ btn, id }) => {
                if (btn) {
                    btn.style.background = normalizedMode === id ? 'var(--BRAND)' : '#898989a1';
                    btn.disabled = false;
                }
            });

            if (tcpServerAddress) {
                tcpServerAddress.style.display = normalizedMode === '1' ? 'flex' : 'none';
            }
            if (tcpRegisterPacket) {
                tcpRegisterPacket.style.display = normalizedMode === '1' ? 'flex' : 'none';
            }
            if (tcpHeartPacket) {
                tcpHeartPacket.style.display = 'flex';
            }
            if (tcpHeartInterval) {
                tcpHeartInterval.style.display = 'flex';
            }

            if (tcpPortInput) {
                tcpPortInput.placeholder = '请输入服务器端口，如：8888';
                if (!preservePortValue) {
                    const trimmed = tcpPortInput.value.trim();
                    if (normalizedMode === '0' && trimmed === '') {
                        tcpPortInput.value = '8888';
                    }
                }
            }

            applyAllPacketFieldStates();
        }

        window.setTcpModeUI = setTcpModeUI;

        function toggleInputs(isDisabled) {
            tcpInputs.forEach(input => input.disabled = isDisabled);
            tcpButtons.forEach(button => button.disabled = isDisabled);
        }

        function tcptoggleInputs() {
            toggleInputs(useTCPState.value === '0');
            applyAllPacketFieldStates();
        }

        useTCPState.addEventListener('change', tcptoggleInputs);
        tcptoggleInputs();

        window.toggleTCPSwitch = function (element) {
            element.classList.toggle('on');
            element.classList.toggle('off');
            const useTCPState = document.getElementById('useTCPState');
            useTCPState.value = element.classList.contains('on') ? '1' : '0';

            // 更新输入框状态
            const isDisabled = !element.classList.contains('on');
            tcpInputs.forEach(input => input.disabled = isDisabled);
            tcpButtons.forEach(button => button.disabled = isDisabled);
            applyAllPacketFieldStates();
        }


        window.changeTCPSelect = function (id) {
            const modeMap = {
                tcpServer: '0',
                tcpClient: '1',
            };
            const mode = modeMap[id] || '1';
            setTcpModeUI(mode);

            const useTCPState = document.getElementById('useTCPState');
            const tcpSwitchBtn = document.getElementById('use_tcp_server');
            if (tcpSwitchBtn.classList.contains('on')) {
                useTCPState.value = '1';
            }
        }

        function convertPacketValue(inputEl, targetFormat) {
            const currentValue = inputEl.value.trim();
            const previousHex = inputEl.dataset.hexBackup || '';
            const previousAscii = inputEl.dataset.asciiBackup || '';
            const convertStatus = inputEl.dataset.convertStatus || '';

            if (targetFormat === 'ascii') {
                if (currentValue) {
                    const asciiValue = hexToAscii(currentValue);
                    if (asciiValue !== currentValue) {
                        inputEl.dataset.hexBackup = currentValue;
                        inputEl.dataset.asciiBackup = asciiValue;
                        inputEl.dataset.convertStatus = 'converted';
                        inputEl.value = asciiValue;
                    } else {
                        inputEl.dataset.hexBackup = currentValue;
                        inputEl.dataset.asciiBackup = '';
                        inputEl.dataset.convertStatus = 'failed';
                        showCustomAlert('已切换为 ASCII 格式，但当前内容无法自动转换，请手动输入有效的 ASCII 文本。', true);
                    }
                } else {
                    inputEl.dataset.hexBackup = '';
                    inputEl.dataset.asciiBackup = '';
                    inputEl.dataset.convertStatus = '';
                }
            } else if (targetFormat === 'hex') {
                let nextValue = currentValue;

                if (convertStatus === 'converted' && previousAscii && currentValue === previousAscii && previousHex) {
                    nextValue = previousHex;
                } else if (convertStatus === 'failed') {
                    if (previousHex && currentValue === previousHex) {
                        nextValue = previousHex;
                    } else {
                        nextValue = asciiToHex(currentValue);
                        inputEl.dataset.hexBackup = nextValue;
                    }
                } else if (currentValue) {
                    nextValue = asciiToHex(currentValue);
                    inputEl.dataset.hexBackup = nextValue;
                }

                inputEl.value = nextValue;
                inputEl.dataset.asciiBackup = '';
                inputEl.dataset.convertStatus = '';
            }
        }

        function setupPacketFormatField(selectId, inputId) {
            const selectEl = document.getElementById(selectId);
            const inputEl = document.getElementById(inputId);
            if (!selectEl || !inputEl) return;

            selectEl.dataset.prevValue = selectEl.value;
            updatePacketInputState(selectEl, inputEl);

            selectEl.addEventListener('change', () => {
                const prevValue = selectEl.dataset.prevValue || 'hex';
                const currentValue = selectEl.value;

                if (currentValue !== 'none' && prevValue !== currentValue && prevValue !== 'none') {
                    convertPacketValue(inputEl, currentValue);
                }

                selectEl.dataset.prevValue = currentValue;
                updatePacketInputState(selectEl, inputEl);
            });
        }

        packetFieldConfigs.forEach(({ selectId, inputId }) => setupPacketFormatField(selectId, inputId));

    // HEX转ASCII函数
    function hexToAscii(hexString) {
        try {
            // 移除空格和非十六进制字符
            const cleanHex = hexString.replace(/[^0-9A-Fa-f]/g, '');
            if (cleanHex.length % 2 !== 0) {
                return hexString; // 如果不是有效的HEX字符串，返回原值
            }

            let ascii = '';
            for (let i = 0; i < cleanHex.length; i += 2) {
                const hexByte = cleanHex.substr(i, 2);
                const charCode = parseInt(hexByte, 16);
                // 只转换可打印字符(32-126)
                if (charCode >= 32 && charCode <= 126) {
                    ascii += String.fromCharCode(charCode);
                } else {
                    return hexString; // 如果包含不可打印字符，返回原值
                }
            }
            return ascii;
        } catch (e) {
            return hexString; // 转换失败返回原值
        }
    }

    // ASCII转HEX函数
    function asciiToHex(asciiString) {
        try {
            let hex = '';
            for (let i = 0; i < asciiString.length; i++) {
                const charCode = asciiString.charCodeAt(i);
                hex += charCode.toString(16).toUpperCase().padStart(2, '0');
            }
            return hex;
        } catch (e) {
            return asciiString; // 转换失败返回原值
        }
    }

    tcpDataForm.addEventListener('submit', event => event.preventDefault());

    async function tcpSubmit() {
        const backendApiUrl = '/tcp';
        const formData = new FormData(tcpDataForm);
        const selectedTcpMode = document.getElementById('selectedTCP')?.value || '1';

        // 检查TCP开关的状态，确保use_tcp值正确
        const tcpSwitchBtn = document.getElementById('use_tcp_server');
        if (tcpSwitchBtn.classList.contains('on')) {
            formData.set('use_tcp', '1');
        }

        // ModbusTCPClient 模式固定无注册包/无心跳包
        if (selectedTcpMode === '3') {
            formData.set('reg_format', 'none');
            formData.set('reg_packet', '');
            formData.set('heart_format', 'none');
            formData.set('heart_packet', '');
            formData.set('heart_interval', '30');
        }

        const json = JSON.stringify(Object.fromEntries(formData.entries()));

        try {
            const data = await fetchData(backendApiUrl, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: json
            });

            setI18nText(tcpSubmitButton, data.code === 200 ? 'message.saveSuccess' : 'message.saveFailed',
                data.code === 200 ? '保存成功' : '保存失败');

            if (data.code === 200) {
                const deviceRestart = document.getElementById('deviceRestart');
                if (deviceRestart) {
                    showSvg('deviceRestart', 6000);
                }
            }

            setTimeout(() => {
                setI18nText(tcpSubmitButton, 'common.submit', '提交');
            }, 2000);
        } catch (error) {
            console.error('Error submitting data:', error);
        }
    }

    tcpSubmitButton.addEventListener('click', tcpSubmit);

    async function tcpfetchData() {
        const apiUrl = '/tcp_info';

        try {
            const responseData = await fetchData(apiUrl);
            document.getElementById('tcp_server').value = responseData.tcp_server;

            // 获取设备MAC地址
            let deviceMac = '';
            try {
                const devInfo = await fetchData('/devinfo');
                // 使用STA MAC地址（去掉冒号）
                if (devInfo.device_sta_mac) {
                    deviceMac = devInfo.device_sta_mac.replace(/:/g, '');
                }
            } catch (error) {
                console.error('Error fetching device info:', error);
            }

            const tcpMode = responseData.tcpconn || '0';

            // 设置注册包和心跳包的值
            // 如果服务器没有返回注册包值，使用设备MAC地址作为默认值
            document.getElementById('reg_packet').value =
                tcpMode === '3' ? '' : (responseData.reg_packet || deviceMac || '');

            // 如果服务器没有返回心跳包值，使用设备MAC地址作为默认值
            document.getElementById('heart_packet').value = responseData.heart_packet
                || (tcpMode === '3' ? '' : (deviceMac || ''));

            if (responseData.heart_interval) {
                document.getElementById('heart_interval').value = responseData.heart_interval;
            } else {
                document.getElementById('heart_interval').value = '30'; // 默认30秒
            }

            const regFormatSelect = document.getElementById('regFormat');
            const heartFormatSelect = document.getElementById('heartFormat');
            const allowedFormats = ['none', 'hex', 'ascii'];

            if (regFormatSelect) {
                const regValue = tcpMode === '3'
                    ? 'none'
                    : (allowedFormats.includes(responseData.reg_format) ? responseData.reg_format : 'hex');
                regFormatSelect.value = regValue || 'hex';
                regFormatSelect.dataset.prevValue = regFormatSelect.value;
            }

            if (heartFormatSelect) {
                const heartValue = allowedFormats.includes(responseData.heart_format)
                    ? responseData.heart_format
                    : (tcpMode === '3' ? 'none' : 'ascii');
                heartFormatSelect.value = heartValue || 'ascii';
                heartFormatSelect.dataset.prevValue = heartFormatSelect.value;
            }

            applyAllPacketFieldStates();

            if (responseData.use_tcp == 0) {
                document.getElementById('use_tcp_server').className = "switch-btn tcp-switch-btn off";
                document.getElementById('useTCPState').value = '0';
                toggleInputs(true);
            } else {
                document.getElementById('use_tcp_server').className = "switch-btn tcp-switch-btn on";
                document.getElementById('useTCPState').value = '1';
                toggleInputs(false);
            }

            const tcpPortInput = document.getElementById('tcp_port');
            const resolvedPort = responseData.tcp_port && responseData.tcp_port.length > 0
                ? responseData.tcp_port
                : ((tcpMode === '2' || tcpMode === '3') ? '502' : '8888');
            if (tcpPortInput) {
                tcpPortInput.value = resolvedPort;
            }

            setTcpModeUI(tcpMode, { preservePortValue: true });

            renderTcpRuntimeStatus(responseData);
        } catch (error) {
            console.error('Failed to fetch data:', error);
        }
    }

    tcpfetchData();
});

// HTTP config API

document.addEventListener('DOMContentLoaded', () => {
    const useHTTPState = document.getElementById('useHTTPState');
    const httpInputs = document.querySelectorAll('.http-input');
    const httpButtons = document.querySelectorAll('.http-btn');
    const httpSubmitButton = document.getElementById('httpSubmitButton');
    const httpDataForm = document.getElementById('httpDataForm');
    const httpSwitchElement = document.getElementById('use_http');

    function toggleInputs(isDisabled) {
        httpInputs.forEach(input => input.disabled = isDisabled);
        httpButtons.forEach(button => button.disabled = isDisabled);
    }

    function httptoggleInputs() {
        toggleInputs(useHTTPState.value === '0');
    }

    useHTTPState.addEventListener('change', httptoggleInputs);
    httptoggleInputs();

    window.toggleHTTPSwitch = function (element) {
        element.classList.toggle('on');
        element.classList.toggle('off');
        useHTTPState.value = element.classList.contains('on') ? '1' : '0';
        httptoggleInputs();
    }

    window.changeHTTPSelect = function (id) {
        httpButtons.forEach(button => button.style.background = '#898989a1');
        if (id === 'http_post') {
            document.getElementById('http_post').style.background = 'var(--BRAND)';
        }
        document.getElementById('selectedHTTP').value = '1';
    }

    httpDataForm.addEventListener('submit', event => event.preventDefault());

    async function httpSubmit() {
        // 注释：移除工作模式检查以避免循环依赖
        // 用户可以先配置HTTP协议，再设置工作模式为Modbus-RTU
        // 后端会在实际使用时进行验证
        
        const fields = [
            { id: 'http_url', errMsg: '请输入有效的HTTP URI，格式如：https://example.com:8080/api', validate: validateHttpUri },
            // { id: 'http_time', errMsg: '请输入间隔时间，且必须大于2秒', validate: validateIntervalTime },
        ];

        for (const field of fields) {
            const input = document.getElementById(field.id);
            const errText = input.nextElementSibling;
            if (!input.value.trim()) {
                errText.textContent = field.errMsg;
                return;
            } else if (field.validate && !field.validate(input.value)) {
                errText.textContent = field.errMsg;
                return;
            } else {
                errText.textContent = '';
            }
        }

        const backendApiUrl = '/http';
        const formData = new FormData(httpDataForm);
        formData.set('use_http', useHTTPState.value);
        const json = JSON.stringify(Object.fromEntries(formData.entries()));
        try {
            const data = await fetchData(backendApiUrl, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: json
            });

            setI18nText(httpSubmitButton, data.code === 200 ? 'message.setSuccess' : 'message.setFailed',
                data.code === 200 ? '设置成功' : '设置失败');
            if (data.code === 200) {
                if (!hasRestarted) {
                    const deviceRestart = document.getElementById('deviceRestart');
                    if (deviceRestart) {
                        showSvg('deviceRestart', 6000);
                        hasRestarted = true;
                    }
                }
            }
            setTimeout(() => {
                setI18nText(httpSubmitButton, 'common.set', '设置');
            }, 2000);
        } catch (error) {
            console.error('Error submitting data:', error);
        }
    }

    httpSubmitButton.addEventListener('click', httpSubmit);

    async function httpfetchData() {
        const apiUrl = '/http_info';

        try {
            const responseData = await fetchData(apiUrl);

            const httpEnabled = responseData.use_http == 1;
            httpSwitchElement.className = `switch-btn http-switch-btn ${httpEnabled ? 'on' : 'off'}`;
            useHTTPState.value = httpEnabled ? '1' : '0';
            httptoggleInputs();

            document.getElementById('http_url').value = responseData.http_url || '';
            // document.getElementById('http_time').value = responseData.http_time || '';
            document.getElementById('http_post').style.background = 'var(--BRAND)';

        } catch (error) {
            console.error('Failed to fetch data:', error);
        }
    }
    httpfetchData();
});

function validateHttpUri(value) {
    const httpPattern = /^(https?:\/\/)((\d{1,3}\.){3}\d{1,3}|([a-zA-Z0-9-]+\.)+[a-zA-Z]{2,})(:\d{1,5})?(\/.*)?$/;
    return httpPattern.test(value);
}


// System data API

const systemElements = {
    version: document.getElementById('version'),
    free_heap_size: document.getElementById('free_heap_size'),
    min_ever_free_heap_size: document.getElementById('min_ever_free_heap_size'),
    time_since_boot: document.getElementById('time_since_boot'),
    res_reason: document.getElementById('res_reason'),
    active_sockets: document.getElementById('active_sockets'),
    nvs_usage: document.getElementById('nvs_usage')
};

function formatTime(microseconds) {
    const totalSeconds = Math.floor(microseconds / 1000000);
    const days = Math.floor(totalSeconds / (24 * 3600));
    const hours = Math.floor((totalSeconds % (24 * 3600)) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;

    return `${days > 0 ? days + " day " : ""}${hours > 0 ? hours + " hour " : ""}${minutes > 0 ? minutes + " minute " : ""}${seconds > 0 ? seconds + " second" : ""}`.trim();
}

function updateBasicInfoRuntimeStatus(data) {
    if (!data) return;
    initBasicInfoElements();

    if (basicInfoElements.uptime) {
        const now = new Date();
        basicInfoElements.uptime.textContent = now.getFullYear() + '-' +
            String(now.getMonth() + 1).padStart(2, '0') + '-' +
            String(now.getDate()).padStart(2, '0') + ' ' +
            String(now.getHours()).padStart(2, '0') + ':' +
            String(now.getMinutes()).padStart(2, '0') + ':' +
            String(now.getSeconds()).padStart(2, '0');
    }

    if (basicInfoElements.eth_ip) {
        basicInfoElements.eth_ip.textContent = data.eth_ip || '';
    }
    if (basicInfoElements.sta_ip) {
        basicInfoElements.sta_ip.textContent = data.sta_ip || '';
    }
    if (basicInfoElements.is_dhcps) {
        basicInfoElements.is_dhcps.textContent = data.is_dhcp == 2 ?
            tr('network.staticIP', 'STATIC(静态IP)') :
            tr('network.dhcp', 'DHCP(动态IP)');
    }
    if (basicInfoElements.net_mode) {
        if (data.network_if === 'WiFi') {
            basicInfoElements.net_mode.textContent = tr('network.wifiMode', 'WIFI(无线连接)');
        } else if (data.network_if === 'Ethernet') {
            basicInfoElements.net_mode.textContent = tr('network.ethernet', '以太网(有线连接)');
        }
    }
    if (basicInfoElements.gateway_status) {
        basicInfoElements.gateway_status.textContent = data.gateway || '---';
    }
    if (basicInfoElements.dns_status) {
        basicInfoElements.dns_status.textContent = data.dns || '---';
    }
    updateApStatusElement(data);
    updateInternetStatusElement(data.internet_status);
}

function updateSystemRuntimeStatus(data) {
    if (!data) return;

    if (systemElements.free_heap_size && data.free_heap_size !== undefined) {
        systemElements.free_heap_size.textContent = Number(data.free_heap_size).toFixed(2) + " KB ";
    }
    if (systemElements.min_ever_free_heap_size && data.min_ever_free_heap_size !== undefined) {
        systemElements.min_ever_free_heap_size.textContent = Number(data.min_ever_free_heap_size).toFixed(2) + " KB ";
    }
    if (systemElements.time_since_boot && data.time_since_boot !== undefined) {
        systemElements.time_since_boot.textContent = formatTime(data.time_since_boot);
    }
    if (systemElements.active_sockets && data.active_sockets !== undefined) {
        systemElements.active_sockets.textContent = data.active_sockets;
    }
}

let runtimeStatusFetchInFlight = false;

async function refreshRuntimeStatus() {
    if (runtimeStatusFetchInFlight) return;
    runtimeStatusFetchInFlight = true;
    try {
        const data = await fetchData('/runtime_status');
        updateBasicInfoRuntimeStatus(data);
        updateSystemRuntimeStatus(data);
        renderMqttRuntimeStatus(data);
        renderTcpRuntimeStatus(data);
    } catch (error) {
        console.error('Failed to refresh runtime status:', error);
    } finally {
        runtimeStatusFetchInFlight = false;
    }
}

document.addEventListener('DOMContentLoaded', () => {
    setTimeout(() => {
        refreshRuntimeStatus();
        setInterval(refreshRuntimeStatus, RUNTIME_STATUS_REFRESH_MS);
    }, 500);
});

let sysFetchInFlight = false;

async function sysfetchData() {
    if (sysFetchInFlight) return;
    sysFetchInFlight = true;
    try {
        const responseData = await fetchData('/sys');
        systemElements.version.textContent = responseData.version;
        systemElements.free_heap_size.textContent = (responseData.free_heap_size).toFixed(2) + " KB ";
        systemElements.min_ever_free_heap_size.textContent = (responseData.min_ever_free_heap_size).toFixed(2) + " KB ";
        systemElements.time_since_boot.textContent = formatTime(responseData.time_since_boot);
        systemElements.res_reason.textContent = responseData.res_reason;
        if (systemElements.active_sockets && responseData.active_sockets !== undefined) {
            systemElements.active_sockets.textContent = responseData.active_sockets;
        }
            // 处理NVS使用率
        if (systemElements.nvs_usage && responseData.nvs_usage !== undefined) {
            if (typeof responseData.nvs_usage === 'number') {
                systemElements.nvs_usage.textContent = responseData.nvs_usage.toFixed(2) + "%";
                // 根据使用率设置颜色警告
                if (responseData.nvs_usage > 80) {
                    systemElements.nvs_usage.style.color = 'red';
                } else if (responseData.nvs_usage > 60) {
                    systemElements.nvs_usage.style.color = 'orange';
                } else {
                    systemElements.nvs_usage.style.color = 'green';
                }
            } else {
                systemElements.nvs_usage.textContent = responseData.nvs_usage;
                systemElements.nvs_usage.style.color = 'gray';
            }
        }
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
        Object.values(systemElements).forEach(element => {
            if (element) element.textContent = "---";
        });
    } finally {
        sysFetchInFlight = false;
    }
}



sysfetchData();

// Module set API

document.getElementById("module_set").addEventListener("submit", event => event.preventDefault());

async function modulesetSubmit() {
    const formData = new FormData(document.getElementById('module_set'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/module_set', data);
        setI18nText('moduleSetButton', 'message.setSuccess', '设置成功');
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        setI18nText('moduleSetButton', 'message.setFailed', '设置失败');
    }
    setTimeout(() => setI18nText('moduleSetButton', 'common.set', '设置'), 2000);
}

document.getElementById('moduleSetButton').addEventListener('click', modulesetSubmit);

async function modulesetfetchData() {
    try {
        const responseData = await fetchData('/module_set_info');
        ['host_names', 'lgname', 'lgpwd'].forEach(id => document.getElementById(id).value = responseData[id]);
        document.getElementById('moduleSetButton').textContent = responseData.host_names != "以太网串口服务器" ? '修改' : '保存';
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}

modulesetfetchData();

// AP duration API

document.getElementById("ap_duration_set").addEventListener("submit", event => event.preventDefault());

async function apDurationSubmit() {
    const formData = new FormData(document.getElementById('ap_duration_set'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/ap_set', data);
        setI18nText('apDurationSetButton', 'message.setSuccess', '设置成功');
    } catch (error) {
        console.error('Error setting AP duration:', error);
        setI18nText('apDurationSetButton', 'message.setFailed', '设置失败');
    }
    setTimeout(() => setI18nText('apDurationSetButton', 'common.set', '设置'), 2000);
}

document.getElementById('apDurationSetButton').addEventListener('click', apDurationSubmit);

async function apDurationFetchData() {
    try {
        const responseData = await fetchData('/ap_set_info');
        const select = document.getElementById('ap_wait_time');
        const value = responseData.ap_wait_time || '0';
        const validValues = ['600', '1200', '1800', '3600', '0'];
        select.value = validValues.includes(value) ? value : '0';
    } catch (error) {
        console.error('Failed to fetch AP duration. Status:', error);
    }
}

apDurationFetchData();

// OTA API

// document.getElementById("otaDataForm").addEventListener("submit", event => event.preventDefault());


async function otaSubmit() {
    const formData = new FormData(document.getElementById('otaDataForm'));
    const data = Object.fromEntries(formData);
    const progressDiv = document.getElementById('otaProgress');
    const progressBar = document.getElementById('progressBar');
    const progressText = document.getElementById('progressText');
    const otaStatus = document.getElementById('otaStatus');

    try {
        // 显示进度条
        progressDiv.style.display = 'block';
        progressBar.style.width = '0%';
        otaStatus.className = 'status-message status-progress';

        document.getElementById('otaSubmitButton').disabled = true;
        document.getElementById('ota_url').disabled = true;

        // 发送OTA请求
        const response = await postData('/ota', data);
        if (response.code !== 200) {
            throw new Error('OTA启动失败');
        }

        // 开始轮询进度
        const checkProgress = async () => {
            try {
                const progressResponse = await fetch('/ota_progress');
                const progressData = await progressResponse.json();

                // 更新进度条
                progressBar.style.width = `${progressData.progress}%`;
                progressText.textContent = `${progressData.progress}%`;
                otaStatus.textContent = progressData.status;

                if (progressData.progress >= 100) {
                    otaStatus.textContent = tr('message.otaSuccessRestart', '更新成功，设备即将重启...');
                    otaStatus.className = 'status-message status-success';
                    progressBar.style.background = 'linear-gradient(90deg, #4CAF50, #45a049)';
                    setTimeout(() => location.reload(), 5000);
                    return;
                }

                if (progressData.status.includes('失败') || progressData.status.includes('error')) {
                    otaStatus.className = 'status-message status-error';
                    progressBar.style.background = '#f44336';
                    document.getElementById('otaSubmitButton').disabled = false;
                    document.getElementById('ota_url').disabled = false;
                    return;
                }

                // 继续轮询
                setTimeout(checkProgress, 1000);
            } catch (error) {
                console.error('获取进度失败:', error);
                otaStatus.textContent = tr('message.otaProgressFailed', '获取升级进度失败');
                otaStatus.className = 'status-message status-error';
                progressBar.style.background = '#f44336';
            }
        };

        // 开始检查进度
        checkProgress();

    } catch (error) {
        console.error('Error:', error);
        progressDiv.style.display = 'none';
        document.getElementById('otaSubmitButton').disabled = false;
        document.getElementById('ota_url').disabled = false;
        otaStatus.textContent = `${tr('message.otaFailed', '升级失败')}: ${error.message}`;
        otaStatus.className = 'status-message status-error';
    }
}

// 确保表单提交事件被正确处理
document.getElementById('otaDataForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    await otaSubmit();
});

document.getElementById('otaSubmitButton').addEventListener('click', otaSubmit);


// Opreate API

const observer = new MutationObserver(mutations => {
    mutations.forEach(mutation => {
        if (mutation.type === 'childList') {
            const deviceRestart = document.getElementById('deviceRestart');
            const deviceReset = document.getElementById('deviceReset');
            if (deviceRestart && deviceRestart.style.display !== 'none') {
                handleDeviceRestart();
            }
            if (deviceReset && deviceReset.style.display !== 'none') {
                handleDeviceReset();
            }
        }
    });
});

const config = { attributes: true, childList: true, subtree: true };
observer.observe(document.body, config);

async function handleDeviceRestart() {
    document.getElementById('cancelRestart').addEventListener('click', () => {
        deviceRestart.style.display = 'none';
    });
    document.getElementById('restart').addEventListener('click', async () => {
        deviceRestart.style.display = 'none';
        try {
            await fetchData('/operate');
            displaySuccessMessage('重启中，请稍后...');
            setTimeout(() => location.reload(), 5000);
        } catch (error) {
            console.error('Error:', error);
        }
    });
}

async function handleDeviceReset() {
    document.getElementById('cancelReset').addEventListener('click', () => {
        deviceReset.style.display = 'none';
    });
    document.getElementById('restore').addEventListener('click', async () => {
        deviceReset.style.display = 'none';
        try {
            await fetchData('/restore');
            displaySuccessMessage('重置中，请稍后...');
            setTimeout(() => location.reload(), 5000);
        } catch (error) {
            console.error('Error:', error);
        }
    });
}

function displaySuccessMessage(message) {
    let svg = document.getElementById('successMessage');
    svg.style.display = 'flex';
    document.getElementById('msgValue').textContent = message;
    const clone = svg.cloneNode(true);
    svg.parentNode.replaceChild(clone, svg);
    svg = clone;
    setTimeout(() => svg.style.display = 'none', 1200);
}

function displayErrorMessage(message) {
    let svg = document.getElementById('successMessage');
    svg.style.display = 'flex';
    document.getElementById('msgValue').textContent = message;
    const clone = svg.cloneNode(true);
    svg.parentNode.replaceChild(clone, svg);
    svg = clone;
    setTimeout(() => svg.style.display = 'none', 1200);
}

function displayInfoMessage(message) {
    if (!message) return;
    displaySuccessMessage(message);
}


// log API

const detailLogTab = document.getElementById('detailLogTab');
const simpleLogTab = document.getElementById('simpleLogTab');
let currentLogType = 'detail';
let ws = null;
let reconnectAttempts = 0;
let logSocketEnabled = false;
const MAX_RECONNECT_ATTEMPTS = 5;
const MAX_LOG_SIZE = 100 * 1024; // 100KB

// 日志存储
let detailLogs = '';
let simpleLogs = '';
let autoScroll = true;

// 添加滚动事件监听
const logContent = document.getElementById('logContent');
if (logContent) {
    logContent.addEventListener('scroll', function () {
        // 检查是否滚动到底部（添加1px的容差）
        const isScrolledToBottom = logContent.scrollHeight - logContent.clientHeight <= logContent.scrollTop + 1;
        autoScroll = isScrolledToBottom;
    });
}

// WebSocket初始化函数
function initWebSocket() {
    try {
        if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
            return;
        }
        logSocketEnabled = true;
        ws = new WebSocket(buildWebSocketUrl('/ws/log'));

        ws.onopen = () => {
            console.log('WebSocket连接已建立');
            reconnectAttempts = 0;
        };

        ws.onmessage = (event) => {
            // 首先尝试解析JSON（串口数据和ack消息）
            try {
                let data = JSON.parse(event.data);

                // 处理uart_ack消息
                if (data.type === 'uart_ack') {
                    const key = buildAckKey(data.transferId || '', data.chunkIndex || 0);
                    const handler = pendingUartAcks.get(key);
                    if (handler) {
                        if (data.success) {
                            handler.resolve();
                        } else {
                            handler.reject(data.message || '串口发送失败');
                        }
                    }
                    return;
                }

                // 处理uart_data消息
                if (data.type === 'uart_data') {
                    handleUartDataMessage(data);
                    return;
                }
            } catch (e) {
                // 不是JSON，继续作为普通日志处理
            }

            // 处理普通日志消息
            const logContent = document.getElementById('logContent');
            if (!logContent) {
                console.warn('找不到logContent元素');
                return;
            }

            let line = event.data.trim();
            if (!line) {
                console.log('收到空消息');
                return;
            }

            // 清理ANSI转义序列和日志前缀
            line = line.replace(/\u001b\[\d+(?:;\d+)*m/g, '')
                .replace(/\\u001b\[\d+(?:;\d+)*m/g, '')
                .replace(/\\n/g, '\n')
                .replace(/\\r/g, '\r')
                .replace(/^[EWID]\s*\(\d+\)\s*/gm, '')
                .trim();

            // 获取当前时间
            const now = new Date();
            const timeStr = `【${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')} ${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}】`;

            // 根据日志类型存储日志
            if (line.includes('[SIMPLE]')) {
                // 移除[SIMPLE]标记并处理格式
                line = line.replace('[SIMPLE]', '').trim();
                // 添加时间戳，去掉多余空格
                line = timeStr + line.replace(/【\s+/g, '【').replace(/\s+】/g, '】');

                // 处理错误日志的颜色
                if (line.toLowerCase().includes('error')) {
                    line = `<span style="color: #d9534f">${line}</span>`;
                }

                // 添加新日志并检查大小
                simpleLogs += line + '\n';
                if (simpleLogs.length > MAX_LOG_SIZE) {
                    const firstNewLine = simpleLogs.indexOf('\n', simpleLogs.length - MAX_LOG_SIZE);
                    if (firstNewLine !== -1) {
                        simpleLogs = simpleLogs.substring(firstNewLine + 1);
                    }
                }

                if (currentLogType === 'simple') {
                    logContent.innerHTML = simpleLogs;
                    if (autoScroll) {
                        logContent.scrollTop = logContent.scrollHeight;
                    }
                }
            } else {
                // 处理详细日志，去掉多余空格
                line = timeStr + (line.includes('【') ?
                    line.replace(/【\s+/g, '【').replace(/\s+】/g, '】') :
                    line);

                // 处理错误日志的颜色
                if (line.toLowerCase().includes('error')) {
                    line = `<span style="color: #d9534f">${line}</span>`;
                }

                // 添加新日志并检查大小
                detailLogs += line + '\n';
                if (detailLogs.length > MAX_LOG_SIZE) {
                    const firstNewLine = detailLogs.indexOf('\n', detailLogs.length - MAX_LOG_SIZE);
                    if (firstNewLine !== -1) {
                        detailLogs = detailLogs.substring(firstNewLine + 1);
                    }
                }

                if (currentLogType === 'detail') {
                    logContent.innerHTML = detailLogs;
                    if (autoScroll) {
                        logContent.scrollTop = logContent.scrollHeight;
                    }
                }
            }
        };

        ws.onclose = () => {
            console.log('WebSocket连接已关闭');
            ws = null;
            if (logSocketEnabled && reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                setTimeout(() => {
                    if (!logSocketEnabled) return;
                    reconnectAttempts++;
                    console.log(`尝试重新连接 (${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})`);
                    initWebSocket();
                }, 3000);
            }
        };

        ws.onerror = (error) => {
            console.error('WebSocket错误:', error);
        };
    } catch (error) {
        console.error('初始化WebSocket时发生错误:', error);
        setTimeout(() => {
            if (logSocketEnabled && reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                reconnectAttempts++;
                console.log(`尝试重新连接 (${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})`);
                initWebSocket();
            }
        }, 3000);
    }
}

function closeLogWebSocket() {
    logSocketEnabled = false;
    reconnectAttempts = 0;
    if (ws) {
        const socket = ws;
        ws = null;
        socket.close();
    }
}

// 切换日志类型的函数
function switchLogType(type) {
    currentLogType = type;
    const logContent = document.getElementById('logContent');

    if (type === 'detail') {
        detailLogTab.style.borderBottom = '2px solid #009ee1';
        detailLogTab.style.color = '#009ee1';
        simpleLogTab.style.borderBottom = 'none';
        simpleLogTab.style.color = '#333';
        logContent.innerHTML = detailLogs;
    } else {
        simpleLogTab.style.borderBottom = '2px solid #009ee1';
        simpleLogTab.style.color = '#009ee1';
        detailLogTab.style.borderBottom = 'none';
        detailLogTab.style.color = '#333';
        logContent.innerHTML = simpleLogs;
    }

    // 切换日志类型时保持当前滚动状态
    if (autoScroll) {
        logContent.scrollTop = logContent.scrollHeight;
    }
}

// 手动滚动到底部的函数
function scrollToBottom() {
    const logContent = document.getElementById('logContent');
    if (logContent) {
        logContent.scrollTop = logContent.scrollHeight;
        autoScroll = true;
    }
}

// 清空日志函数
async function clearLog() {
    try {
        const response = await fetch('/clear_log');
        if (response.ok) {
            if (currentLogType === 'detail') {
                detailLogs = '';
            } else {
                simpleLogs = '';
            }
            document.getElementById('logContent').innerHTML = '';
            displaySuccessMessage(translations[currentLang].clearLogSuccess);
        } else {
            throw new Error('Failed to clear log');
        }
    } catch (error) {
        console.error('Failed to clear log:', error);
        displayErrorMessage(translations[currentLang].clearLogFail);
    }
}

// 导出日志函数
async function exportLog() {
    try {
        const logContent = document.getElementById('logContent');
        if (!logContent || !logContent.innerHTML) {
            displayErrorMessage(translations[currentLang].noLog || '暂无日志');
            return;
        }

        const tempDiv = document.createElement('div');
        tempDiv.innerHTML = logContent.innerHTML;
        let logText = tempDiv.textContent;

        const blob = new Blob([logText], { type: 'text/plain' });
        const url = window.URL.createObjectURL(blob);
        const link = document.createElement('a');

        const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
        const filename = `WXNG1_${timestamp}${currentLogType === 'simple' ? '_simple' : ''}.log`;

        link.href = url;
        link.download = filename;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        window.URL.revokeObjectURL(url);

        displaySuccessMessage(translations[currentLang].exportLogSuccess || '导出日志成功');
    } catch (error) {
        console.error('导出日志失败:', error);
        displayErrorMessage(translations[currentLang].exportLogFail || '导出日志失败');
    }
}

// 日志开关切换函数
window.toggleLogSwitch = function (element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    const enabled = element.classList.contains('on');
    document.getElementById('logEnabledState').value = enabled ? '1' : '0';

    // 调用API保存状态
    saveLogSwitchState(enabled);
}

// 保存日志开关状态到后端
async function saveLogSwitchState(enabled) {
    try {
        const data = await fetchData('/api/log_switch', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                log_enabled: enabled
            })
        });

        if (data.code === 200) {
            console.log('日志开关状态已保存:', enabled ? '开启' : '关闭');
            displaySuccessMessage(enabled ? '日志已开启' : '日志已关闭');
        } else {
            console.error('保存日志开关状态失败');
            displayErrorMessage('保存失败');
        }
    } catch (error) {
        console.error('保存日志开关状态出错:', error);
        displayErrorMessage('保存失败');
    }
}

// 加载日志开关状态
async function loadLogSwitchState() {
    try {
        const data = await fetchData('/api/log_switch');

        if (data.code === 200) {
            const switchElement = document.getElementById('logEnableSwitch');
            const stateInput = document.getElementById('logEnabledState');

            if (switchElement && stateInput) {
                if (data.log_enabled) {
                    switchElement.classList.remove('off');
                    switchElement.classList.add('on');
                    stateInput.value = '1';
                } else {
                    switchElement.classList.remove('on');
                    switchElement.classList.add('off');
                    stateInput.value = '0';
                }
                console.log('日志开关状态已加载:', data.log_enabled ? '开启' : '关闭');
            }
        }
    } catch (error) {
        console.error('加载日志开关状态出错:', error);
    }
}

// 页面加载完成后初始化
document.addEventListener('DOMContentLoaded', function () {
    // 延后初始化WebSocket，避免与首屏配置接口同时抢占AP上的HTTP连接。
    setTimeout(initWebSocket, 1500);

    // 初始化日志标签页样式
    if (detailLogTab && simpleLogTab) {
        detailLogTab.style.borderBottom = '2px solid #009ee1';
        detailLogTab.style.color = '#009ee1';
        simpleLogTab.style.borderBottom = 'none';
        simpleLogTab.style.color = '#333';
    }

    // 设置事件监听器
    detailLogTab?.addEventListener('click', () => switchLogType('detail'));
    simpleLogTab?.addEventListener('click', () => switchLogType('simple'));
    document.getElementById('clearLogButton')?.addEventListener('click', clearLog);
    document.getElementById('exportLogButton')?.addEventListener('click', exportLog);

    // 加载日志开关状态
    loadLogSwitchState();

    // 设置日志配置按钮事件
    const logConfig = document.getElementById('logConfig');
    const logConfig_m = document.getElementById('logConfig_m');

    [logConfig, logConfig_m].forEach(element => {
        element?.addEventListener('click', () => {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                initWebSocket();
            }
        });
    });

    window.addEventListener('beforeunload', closeLogWebSocket);
});

// System banner tree

window.onload = function () {
    // Password visibility toggle
    const passwordFields = [
        { input: 'lgpwd', openEye: 'eyeOpen', closedEye: 'eyeClosed' },
        { input: 'mqtt_password', openEye: 'mqtteyeOpen', closedEye: 'mqtteyeClosed' },
        { input: 'wifi_password', openEye: 'wifieyeOpen', closedEye: 'wifieyeClosed' }
    ];

    function togglePasswordVisibility(input, openEye, closedEye) {
        input.type = input.type === 'password' ? 'text' : 'password';
        openEye.style.display = input.type === 'password' ? 'none' : 'block';
        closedEye.style.display = input.type === 'password' ? 'block' : 'none';
    }

    passwordFields.forEach(field => {
        const input = document.getElementById(field.input);
        const openEye = document.getElementById(field.openEye);
        const closedEye = document.getElementById(field.closedEye);
        openEye.addEventListener('click', () => togglePasswordVisibility(input, openEye, closedEye));
        closedEye.addEventListener('click', () => togglePasswordVisibility(input, openEye, closedEye));
    });

    // Form validation
    var otaUrlInput = document.getElementById('ota_url');
    var otaSubmitButton = document.getElementById('otaSubmitButton');
    otaUrlInput.addEventListener('input', function () {
        if (otaUrlInput.value) {
            otaSubmitButton.disabled = false;
        } else {
            otaSubmitButton.disabled = true;
        }
    });
    var hostNamesInput = document.getElementById('host_names');
    var lgnameInput = document.getElementById('lgname');
    var lgpwdInput = document.getElementById('lgpwd');
    var moduleSetButton = document.getElementById('moduleSetButton');
    function checkInput() {
        if (hostNamesInput.value && lgnameInput.value && lgpwdInput.value) {
            moduleSetButton.disabled = false;
        } else {
            moduleSetButton.disabled = true;
        }
    }
    hostNamesInput.addEventListener('input', checkInput);
    lgnameInput.addEventListener('input', checkInput);
    lgpwdInput.addEventListener('input', checkInput);
    checkInput();
};

function toggleClass(elementId, ...classNames) {
    var element = document.getElementById(elementId);
    classNames.forEach(function (className) {
        element.classList.toggle(className);
    });
}

// Switch view
function switchView(viewId, linkId, mainClass, mainTitle, isMobile) {
    // 获取所有视图
    const views = document.querySelectorAll('[id$="View"]');
    const links = document.querySelectorAll('.nav-link');
    const main = document.getElementById('main');
    const mainTitleElement = document.getElementById('mainTitle');

    // 先将所有视图设置为不可见，并移除动画类
    views.forEach(view => {
        if (view.id !== viewId) {
            view.style.opacity = '0';
            setTimeout(() => {
                view.style.display = 'none';
                view.classList.remove('active');
            }, 300); // 等待淡出动画完成
        }
    });

    // 移除所有链接的active类
    links.forEach(link => link.classList.remove('active', 'router-link-active', 'router-link-exact-active'));

    // 获取目标视图和链接
    const targetView = document.getElementById(viewId);
    const targetLink = document.getElementById(linkId);

    if (!targetView || !targetLink || !main) return;

    // 更新链接状态和主要类
    targetLink.classList.add('active', 'router-link-active', 'router-link-exact-active');
    main.className = mainClass;

    // 更新页面标题 - 使用国际化
    if (mainTitleElement) {
        // 根据mainTitle查找对应的翻译键
        const titleMap = {
            '基本信息': 'nav.basicInfo',
            '串口管理': 'nav.serialConfig',
            '系统管理': 'nav.sysConfig',
            '协议管理': 'nav.protocolConfig',
            '网络管理': 'nav.netConfig',
            '日志管理': 'nav.logConfig'
        };

        const i18nKey = titleMap[mainTitle];
        if (i18nKey && window.i18n) {
            mainTitleElement.textContent = window.i18n.t(i18nKey);
        } else {
            mainTitleElement.textContent = mainTitle;
        }
    }

    // 显示目标视图并添加动画
    targetView.style.display = 'block';

    // 触发重排以确保动画正常运行
    void targetView.offsetWidth;

    // 添加active类以触发动画
    setTimeout(() => {
        targetView.style.opacity = '1';
        targetView.classList.add('active');
    }, 10);

    // 为视图内的所有卡片添加动画
    const cards = targetView.querySelectorAll('.card-view');
    cards.forEach((card, index) => {
        card.style.opacity = '0';
        card.style.transform = 'translateY(20px)';
        setTimeout(() => {
            card.style.opacity = '1';
            card.style.transform = 'translateY(0)';
            card.classList.add('show');
        }, 100 + index * 100); // 级联动画效果
    });
}

// 更新CSS样式
const style = document.createElement('style');
style.textContent = `
    [id$="View"] {
        transition: opacity 0.3s ease-out;
        opacity: 0;
    }

    [id$="View"].active {
        opacity: 1;
    }

    .card-view {
        transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    }

    .nav-link {
        transition: all 0.3s ease;
    }

    .nav-link.active {
        color: var(--BRAND);
        background: var(--BG-1);
    }
`;
document.head.appendChild(style);

// 页面加载完成后初始化动画
document.addEventListener('DOMContentLoaded', () => {
    const currentView = document.querySelector('[id$="View"]:not([style*="display: none"])');
    if (currentView) {
        currentView.classList.add('active');
        currentView.style.opacity = '1';

        const cards = currentView.querySelectorAll('.card-view');
        cards.forEach((card, index) => {
            setTimeout(() => {
                card.style.opacity = '1';
                card.style.transform = 'translateY(0)';
                card.classList.add('show');
            }, index * 100);
        });
    }
});

function addEventListenerToElement(elementId, viewId, linkId, mainClass, mainTitle, isMobile) {
    document.getElementById(elementId).addEventListener("click", function () {
        switchView(viewId, linkId, mainClass, mainTitle, isMobile);
        if (isMobile) {
            setTimeout(function () {
                toggleClass("nav-top", "nav-off", "nav-on");
                toggleClass("app-icon", "icondaohang", "iconchahao");
                toggleClass("nav-lists", "nav-on");
                toggleClass("version-div", "nav-on");
            }, 100);
        }
    });
}

document.getElementById("app-icon-btn").addEventListener("click", function () {
    toggleClass("nav-top", "nav-off", "nav-on");
    toggleClass("app-icon", "icondaohang", "iconchahao");
    toggleClass("nav-lists", "nav-on");
    toggleClass("version-div", "nav-on");
});

function showSvg(svgId, duration) {
    var svg = document.getElementById(svgId);
    svg.style.display = 'flex';
    var clone = svg.cloneNode(true);
    svg.parentNode.replaceChild(clone, svg);
    svg = clone;
    setTimeout(function () {
        svg.style.display = 'none';
    }, duration);
}

addEventListenerToElement("basicInfo", 'infoView', 'basicInfo', 'base-info', '基本信息', false);
addEventListenerToElement("basicInfo_m", 'infoView', 'basicInfo', 'base-info', '基本信息', true);
addEventListenerToElement("serialConfig", 'serialView', 'serialConfig', 'serial', '串口管理', false);
addEventListenerToElement("serialConfig_m", 'serialView', 'serialConfig', 'serial', '串口管理', true);
addEventListenerToElement("sysConfig", 'sysView', 'sysConfig', 'security', '系统管理', false);
addEventListenerToElement("sysConfig_m", 'sysView', 'sysConfig', 'security', '系统管理', true);
addEventListenerToElement("protocolConfig", 'protocolView', 'protocolConfig', 'security', '协议管理', false);
addEventListenerToElement("protocolConfig_m", 'protocolView', 'protocolConfig', 'security', '协议管理', true);
addEventListenerToElement("netConfig", 'netView', 'netConfig', 'base-info', '网络管理', false);
addEventListenerToElement("netConfig_m", 'netView', 'netConfig', 'base-info', '网络管理', true);
addEventListenerToElement("logConfig", 'logView', 'logConfig', 'security', '日志管理', false);
addEventListenerToElement("logConfig_m", 'logView', 'logConfig', 'security', '日志管理', true);



document.getElementById("serialButton").addEventListener("click", function () {
    showSvg('successMessage', 1200);
});

document.getElementById("restartButton").addEventListener("click", function () {
    showSvg('deviceRestart', 6000);
});

document.getElementById("resetButton").addEventListener("click", function () {
    showSvg('deviceReset', 6000);
});

let currentLang = 'zh';

// i18n.js
const translations = {
    en: {
        basic_info: "Basic Information",
        sensor_info: "Sensor Information",
        device_name: "Device Name",
        current_time: "Current Time",
        net_mode: "Network Mode",
        eth_mac: "Ethernet MAC",
        eth_ip: "Ethernet IP Address",
        wifi_mac: "WiFi MAC",
        wifi_ip: "WiFi IP Address",
        ip_acquisition: "IP Acquisition Method",
        sensor_dashboard: "Sensor Dashboard",
        network_settings: "Network Settings",
        network_mode_selection: "Network Mode Selection",
        ethernet: "Ethernet",
        wifi: "WiFi",
        static_ip: "Static IP",
        wifi_name: "WiFi Name",
        wifi_password: "WiFi Password",
        scan_list: "Scan List",
        signal_strength: "Signal Strength",
        ip_address: "IP Address",
        subnet_mask: "Subnet Mask",
        gateway: "Gateway",
        search: "Search",
        submit: "Submit",
        mqttSettings: "MQTT Settings",
        mqttsubmit: "Submit",
        mqttenable: "Enable",
        mqttServerAddress: "Server Address",
        mqttServerPort: "Server Port",
        mqttUsername: "Username",
        mqttPassword: "Password",
        mqttClientID: "Client ID",
        subscribeTopic: "Subscribe Topic",
        publishTopic: "Publish Topic",
        qos: "QoS",
        retain: "Retain",
        timedReport: "Timed Report",
        mqttIntervalTime: "Interval Time",
        mqttConnectionStatus: "Connection Status",
        mqttnConnected: "Connected",
        tcpSettings: "TCP Settings",
        tcpSubmit: "Submit",
        tcpUse: "Enable",
        tcpProtocolType: "Protocol Type",
        tcpServer: "TCP Server",
        tcpClient: "TCP Client",
        modbusTcp: "ModbusTCPServer",
        modbusTcpClient: "ModbusTCPClient",
        tcpIntervalTime: "Interval Time",
        tcpAddress: "Server Address",
        tcpPort: "Server Port",
        tcpConn: "Connection Status",
        tcpnConn: "Connected",
        httpSettings: "HTTP/HTTPS Settings",
        requestType: "Request Type",
        http: "HTTP",
        https: "HTTPS",
        post: "POST",
        get: "GET",
        deviceTitle: "Serial Device Server",
        deviceBasicInfo: "Basic Information",
        deviceConfigurationDashbord: "Configuration Dashboard",
        deviceProtocolConfig: "Protocol Management",
        deviceWiFiConfig: "Network Management",
        deviceAdvancedSet: "Advanced Settings",
        deviceSysConfig: "System Management",
        shuoxinWEB: "SHUOXIN-WEB",
        ztList: "Configuration Dashboard",
        deviceHDvalue: "Hardware Parameters",
        sysVeren: "System Version",
        deviceFreeHeap: "Available Heap Memory",
        deviceMinHeap: "Minimum Available Heap Size",
        deviceRunTime: "System Uptime",
        deviceResReason: "Last Restart Reason",
        loginSet: "Login Settings",
        loginSubmit: "Set",
        deviceName: "Device Name",
        deviceUname: "Username",
        devicePwd: "Login Password",
        deviceOTA: "OTA Firmware Upgrade",
        otaSubmit: "Upgrade",
        otaURL: "OTA URL",
        deviceRestart: "Restart Device",
        isrestart: "Restart",
        deviceReset: "Reset Device",
        isreset: "Reset",
        isdreset: "Restart Device",
        isqreset: "Are you sure you want to restart?",
        dcancel: "Cancel",
        dreq: "Confirm",
        isdres: "Reset Device",
        isqres: "Are you sure you want to reset? The device will be reset after confirmation.",
        scancel: "Cancel",
        sreq: "Confirm",
        opsuccess: "Operation Successful",
        setopsuccess: "Settings Updated Successfully",
        valueCompensate: "Parameter Compensation",
        vcSubmit: "Set",
        httpConn: "HTTP/HTTPS Settings",
        httpSet: "Settings",
        httpIsUse: "Enable",
        httpWay: "Request Method",
        http_time: "Interval Time",
        second: "Second",
        thpsensor: 'Sensor',
        deviceLogConfig: "Log Management",
        logManage: "Log Management",
        clearLog: "Clear Log",
        noLog: "No logs available",
        clearLogSuccess: "Logs cleared successfully",
        clearLogFail: "Failed to clear logs"
    },
    zh: {
        basic_info: "设备状态",
        sensor_info: "传感器信息",
        device_name: "设备名称",
        current_time: "当前时间",
        net_mode: "网络模式",
        eth_mac: "以太网MAC",
        eth_ip: "以太网IP地址",
        wifi_mac: "WiFi MAC",
        wifi_ip: "WiFi IP地址",
        ip_acquisition: "IP获取方式",
        sensor_dashboard: "传感器仪表",
        network_settings: "网络设置",
        network_mode_selection: "网络模式选择",
        ethernet: "以太网",
        wifi: "WiFi",
        static_ip: "静态IP",
        wifi_name: "WIFI名称",
        wifi_password: "WIFI密码",
        scan_list: "扫描列表",
        signal_strength: "信号强度",
        ip_address: "IP地址",
        subnet_mask: "子网掩码",
        gateway: "网关",
        search: "搜索",
        submit: "设置",
        mqttSettings: "MQTT设置",
        mqttsubmit: "设置",
        mqttenable: "是否启用",
        mqttServerAddress: "服务器地址",
        mqttServerPort: "服务器端口",
        mqttUsername: "用户名",
        mqttPassword: "密码",
        mqttClientID: "ClientID",
        subscribeTopic: "订阅主题",
        publishTopic: "发布主题",
        qos: "QOS",
        retain: "保留(Retain)",
        timedReport: "定时上报",
        mqttIntervalTime: "间隔时间",
        mqttConnectionStatus: "连接状态",
        mqttnConnected: "未连接",
        tcpSettings: "TCP设置",
        tcpSubmit: "设置",
        tcpUse: "是否启用",
        tcpProtocolType: "协议类型",
        tcpServer: "TCPServer",
        tcpClient: "TCPClient",
        modbusTcp: "ModbusTCPServer",
        modbusTcpClient: "ModbusTCPClient",
        tcpIntervalTime: "间隔时间",
        tcpAddress: "服务器地址",
        tcpPort: "服务器端口",
        tcpConn: "连接状态",
        tcpnConn: "未连接",
        httpSettings: "HTTP/HTTPS设置",
        requestType: "请求方式",
        http: "HTTP",
        https: "HTTPS",
        post: "POST",
        get: "GET",
        deviceTitle: "物小能G1",
        deviceBasicInfo: "基本信息",
        deviceConfigurationDashbord: "组态仪表",
        deviceProtocolConfig: "协议管理",
        deviceWiFiConfig: "网络管理",
        deviceAdvancedSet: "高级设置",
        deviceSysConfig: "系统管理",
        shuoxinWEB: "硕芯官网",
        ztList: "组态仪表",
        deviceHDvalue: "硬件参数",
        sysVeren: "系统版本",
        deviceFreeHeap: "系统可用堆内存",
        deviceMinHeap: "最小可用堆大小",
        deviceRunTime: "系统运行时间",
        deviceResReason: "上一次重启原因",
        loginSet: "登录设置",
        loginSubmit: "设置",
        deviceName: "设备名称",
        deviceUname: "用户名",
        devicePwd: "登录密码",
        deviceOTA: "OTA固件升级",
        otaSubmit: "升级",
        otaURL: "OTA地址",
        deviceRestart: "重启设备",
        isrestart: "重启",
        deviceReset: "恢复出厂参数",
        isreset: "重置",
        isdreset: "重启设备",
        isqreset: "是否确定重启？",
        dcancel: "取消",
        dreq: "确认",
        isdres: "恢复出厂参数",
        isqres: "是否确定重置？确认后设备将被重置",
        scancel: "取消",
        sreq: "确认",
        opsuccess: "操作成功",
        setopsuccess: "设置参数成功",
        valueCompensate: "参数补偿",
        vcSubmit: "设置",
        httpConn: "HTTP/HTTPS设置",
        httpSet: "设置",
        httpIsUse: "是否启用",
        httpWay: "请求方式",
        http_time: "间隔时间",
        second: "秒",
        thpsensor: "传感器",
        deviceLogConfig: "日志管理",
        logManage: "日志管理",
        clearLog: "清除日志",
        noLog: "暂无日志",
        clearLogSuccess: "清除日志成功",
        clearLogFail: "清除日志失败"
    }
};

// 添加自定义提示框函数
// 创建美观的确认对话框
function showCustomConfirm(message, onConfirm, onCancel) {
    // 创建遮罩层
    const overlay = document.createElement('div');
    overlay.style.position = 'fixed';
    overlay.style.top = '0';
    overlay.style.left = '0';
    overlay.style.width = '100%';
    overlay.style.height = '100%';
    overlay.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
    overlay.style.zIndex = '10000';
    overlay.style.display = 'flex';
    overlay.style.justifyContent = 'center';
    overlay.style.alignItems = 'center';

    // 创建对话框容器
    const confirmBox = document.createElement('div');
    confirmBox.style.backgroundColor = '#fff';
    confirmBox.style.borderRadius = '12px';
    confirmBox.style.boxShadow = '0 8px 32px rgba(0, 0, 0, 0.3)';
    confirmBox.style.padding = '0';
    confirmBox.style.minWidth = '400px';
    confirmBox.style.maxWidth = '500px';
    confirmBox.style.fontFamily = '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif';
    confirmBox.style.overflow = 'hidden';
    confirmBox.style.transform = 'scale(0.9)';
    confirmBox.style.transition = 'all 0.3s ease';

    // 创建头部
    const header = document.createElement('div');
    header.style.padding = '24px 24px 16px 24px';
    header.style.borderBottom = '1px solid #e5e5e5';

    const title = document.createElement('h3');
    title.textContent = tr('message.pollingAdjustTitle', '轮询时间调整');
    title.style.margin = '0';
    title.style.fontSize = '18px';
    title.style.fontWeight = '600';
    title.style.color = '#333';
    title.style.display = 'flex';
    title.style.alignItems = 'center';

    const icon = document.createElement('span');
    icon.innerHTML = '⚠️';
    icon.style.marginRight = '8px';
    icon.style.fontSize = '20px';

    title.prepend(icon);
    header.appendChild(title);

    // 创建内容区域
    const content = document.createElement('div');
    content.style.padding = '20px 24px';
    content.style.lineHeight = '1.6';
    content.style.color = '#555';
    content.style.fontSize = '14px';
    content.innerHTML = message;

    // 创建按钮区域
    const buttonArea = document.createElement('div');
    buttonArea.style.padding = '16px 24px 24px 24px';
    buttonArea.style.display = 'flex';
    buttonArea.style.gap = '12px';
    buttonArea.style.justifyContent = 'flex-end';

    // 创建取消按钮
    const cancelButton = document.createElement('button');
    cancelButton.textContent = tr('common.cancel', '取消');
    cancelButton.style.backgroundColor = '#f5f5f5';
    cancelButton.style.color = '#666';
    cancelButton.style.border = '1px solid #ddd';
    cancelButton.style.borderRadius = '8px';
    cancelButton.style.padding = '10px 20px';
    cancelButton.style.fontSize = '14px';
    cancelButton.style.cursor = 'pointer';
    cancelButton.style.transition = 'all 0.2s ease';
    cancelButton.style.fontWeight = '500';

    cancelButton.onmouseover = function() {
        this.style.backgroundColor = '#e8e8e8';
        this.style.borderColor = '#ccc';
    };
    cancelButton.onmouseout = function() {
        this.style.backgroundColor = '#f5f5f5';
        this.style.borderColor = '#ddd';
    };

    // 创建确认按钮
    const confirmButton = document.createElement('button');
    confirmButton.textContent = tr('message.autoAdjust', '自动调整');
    confirmButton.style.backgroundColor = '#007bff';
    confirmButton.style.color = '#fff';
    confirmButton.style.border = 'none';
    confirmButton.style.borderRadius = '8px';
    confirmButton.style.padding = '10px 20px';
    confirmButton.style.fontSize = '14px';
    confirmButton.style.cursor = 'pointer';
    confirmButton.style.transition = 'all 0.2s ease';
    confirmButton.style.fontWeight = '500';

    confirmButton.onmouseover = function() {
        this.style.backgroundColor = '#0056b3';
        this.style.transform = 'translateY(-1px)';
    };
    confirmButton.onmouseout = function() {
        this.style.backgroundColor = '#007bff';
        this.style.transform = 'translateY(0)';
    };

    // 关闭对话框函数
    function closeDialog() {
        confirmBox.style.transform = 'scale(0.9)';
        overlay.style.opacity = '0';
        setTimeout(() => {
            document.body.removeChild(overlay);
        }, 300);
    }

    // 绑定事件
    cancelButton.onclick = function() {
        closeDialog();
        if (onCancel) onCancel();
    };

    confirmButton.onclick = function() {
        closeDialog();
        if (onConfirm) onConfirm();
    };

    // 按ESC关闭
    const handleEscape = function(e) {
        if (e.key === 'Escape') {
            closeDialog();
            if (onCancel) onCancel();
            document.removeEventListener('keydown', handleEscape);
        }
    };
    document.addEventListener('keydown', handleEscape);

    // 组装对话框
    buttonArea.appendChild(cancelButton);
    buttonArea.appendChild(confirmButton);
    confirmBox.appendChild(header);
    confirmBox.appendChild(content);
    confirmBox.appendChild(buttonArea);
    overlay.appendChild(confirmBox);

    // 添加到页面
    document.body.appendChild(overlay);

    // 显示动画
    setTimeout(() => {
        overlay.style.opacity = '1';
        confirmBox.style.transform = 'scale(1)';
    }, 10);

    // 自动聚焦确认按钮
    setTimeout(() => {
        confirmButton.focus();
    }, 300);
}

function showCustomAlert(message, isError = false) {
    // 如果已有提示框，先移除所有相关元素
    const existingAlerts = document.querySelectorAll('#customAlertBox');
    const existingOverlays = document.querySelectorAll('.custom-alert-overlay');
    
    existingAlerts.forEach(alert => {
        if (alert.parentNode) {
            document.body.removeChild(alert);
        }
    });
    
    existingOverlays.forEach(overlay => {
        if (overlay.parentNode) {
            document.body.removeChild(overlay);
        }
    });
    
    // 等待DOM清理完成
    setTimeout(() => createAlert(), 10);
    
    function createAlert() {
        // 创建提示框容器
        const alertBox = document.createElement('div');
        alertBox.id = 'customAlertBox';
        alertBox.style.position = 'fixed';
        alertBox.style.top = '50%';
        alertBox.style.left = '50%';
        alertBox.style.transform = 'translate(-50%, -50%)';
        alertBox.style.zIndex = '10001'; // 提高z-index确保在最顶层
        alertBox.style.backgroundColor = '#fff';
        alertBox.style.borderRadius = '5px';
        alertBox.style.boxShadow = '0 0 20px rgba(0, 0, 0, 0.3)';
        alertBox.style.padding = '20px 25px';
        alertBox.style.minWidth = '350px';
        alertBox.style.maxWidth = '80%';
        alertBox.style.textAlign = 'center';
        alertBox.style.border = isError ? '1px solid #d9534f' : '1px solid #2b6ec0';
        alertBox.style.fontFamily = '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif';

        // 添加标题
        const title = document.createElement('div');
        title.style.fontWeight = 'bold';
        title.style.fontSize = '18px';
        title.style.marginBottom = '15px';
        title.style.color = isError ? '#d9534f' : '#2b6ec0';
        title.textContent = isError ? '错误提示' : '温馨提示';
        alertBox.appendChild(title);

        // 添加图标
        const icon = document.createElement('div');
        icon.style.fontSize = '24px';
        icon.style.marginBottom = '10px';
        icon.innerHTML = isError ? '&#9888;' : '&#8505;'; // 警告图标或信息图标
        icon.style.color = isError ? '#d9534f' : '#2b6ec0';
        title.insertBefore(icon, title.firstChild);

        // 添加分隔线
        const divider = document.createElement('div');
        divider.style.height = '1px';
        divider.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
        divider.style.opacity = '0.3';
        divider.style.margin = '0 -25px 15px -25px';
        alertBox.appendChild(divider);

        // 添加消息内容
        const content = document.createElement('div');
        content.style.marginBottom = '20px';
        content.style.color = '#333';
        content.style.fontSize = '15px';
        content.style.lineHeight = '1.5';
        const normalizedMessage = (message || '').toString().replace(/<br\s*\/?>/gi, '\n');
        const safeMessage = normalizedMessage
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/\n/g, '<br>');
        content.innerHTML = safeMessage;
        alertBox.appendChild(content);

        // 添加确定按钮
        const confirmButton = document.createElement('button');
        confirmButton.textContent = tr('common.ok', '确定');
        confirmButton.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
        confirmButton.style.color = '#fff';
        confirmButton.style.border = 'none';
        confirmButton.style.borderRadius = '5px';
        confirmButton.style.padding = '10px 20px';
        confirmButton.style.fontSize = '14px';
        confirmButton.style.cursor = 'pointer';
        confirmButton.style.transition = 'all 0.2s ease';
        confirmButton.style.boxShadow = '0 2px 5px rgba(0,0,0,0.2)';

        // 添加鼠标悬停效果
        confirmButton.onmouseover = function() {
            this.style.backgroundColor = isError ? '#c9302c' : '#1a5aa0';
            this.style.boxShadow = '0 4px 8px rgba(0,0,0,0.2)';
        };
        confirmButton.onmouseout = function() {
            this.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
            this.style.boxShadow = '0 2px 5px rgba(0,0,0,0.2)';
        };

        confirmButton.onclick = function() {
            // 淡出效果
            alertBox.style.opacity = '0';
            overlay.style.opacity = '0';
            setTimeout(() => {
                document.body.removeChild(alertBox);
                document.body.removeChild(overlay);
            }, 300);
        };
        alertBox.appendChild(confirmButton);

        // 添加遮罩层
        const overlay = document.createElement('div');
        overlay.className = 'custom-alert-overlay';
        overlay.style.position = 'fixed';
        overlay.style.top = '0';
        overlay.style.left = '0';
        overlay.style.width = '100%';
        overlay.style.height = '100%';
        overlay.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
        overlay.style.zIndex = '10000'; // 保持与confirmButton一致

        // 添加到文档中
        document.body.appendChild(overlay);
        document.body.appendChild(alertBox);

        // 设置淡入效果
        alertBox.style.opacity = '0';
        overlay.style.opacity = '0';
        setTimeout(() => {
            alertBox.style.transition = 'opacity 0.3s ease';
            overlay.style.transition = 'opacity 0.3s ease';
            alertBox.style.opacity = '1';
            overlay.style.opacity = '1';
        }, 10);

        // 点击遮罩层关闭提示框
        overlay.onclick = function() {
            alertBox.style.opacity = '0';
            overlay.style.opacity = '0';
            setTimeout(() => {
                document.body.removeChild(alertBox);
                document.body.removeChild(overlay);
            }, 300);
        };

        // 添加键盘事件监听，按ESC键关闭
        const escKeyHandler = function(e) {
            if (e.key === 'Escape') {
                confirmButton.click();
                document.removeEventListener('keydown', escKeyHandler);
            }
        };
        document.addEventListener('keydown', escKeyHandler);

        // 返回overlay和alertBox，方便后续操作
        return {
            overlay,
            alertBox,
            close: function() {
                document.body.removeChild(alertBox);
                document.body.removeChild(overlay);
                document.removeEventListener('keydown', escKeyHandler);
            }
        };
    } // createAlert函数的结尾
}

// 显示多协议选择对话框（支持用户勾选要启用的协议）
function showProtocolSelectionDialog(options) {
    return new Promise((resolve) => {
        const { title, message, protocols, onConfirm, onCancel } = options;
        
        // 创建遮罩层
        const overlay = document.createElement('div');
        overlay.style.cssText = `
            position: fixed; top: 0; left: 0; width: 100%; height: 100%;
            background-color: rgba(136, 136, 136, 0.3); z-index: 9999999;
        `;
        
        // 创建对话框容器
        const dialog = document.createElement('div');
        dialog.style.cssText = `
            position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%);
            width: 90%; max-width: 450px; background: var(--BG-1);
            box-shadow: 0 20px 60px 0 rgba(0, 0, 0, 0.1);
            border-radius: var(--RADIUS-0); z-index: 99999999;
            padding: 32px;
        `;
        
        // 标题
        const titleElem = document.createElement('h3');
        titleElem.textContent = title;
        titleElem.style.cssText = `
            margin: 0 0 16px 0; font-size: 18px; line-height: 26px;
            color: var(--FG-0); text-align: center;
        `;
        dialog.appendChild(titleElem);
        
        // 消息内容
        const messageElem = document.createElement('p');
        messageElem.innerHTML = message.replace(/\n/g, '<br>');
        messageElem.style.cssText = `
            margin: 0 0 20px 0; font-size: 14px; line-height: 20px;
            color: var(--FG-2);
        `;
        dialog.appendChild(messageElem);
        
        // 协议选择区域
        const protocolsContainer = document.createElement('div');
        protocolsContainer.style.cssText = `
            background: var(--BG-AREA); border-radius: var(--RADIUS-1);
            padding: 16px; margin-bottom: 20px;
        `;
        
        const protocolsTitle = document.createElement('div');
        protocolsTitle.textContent = tr('message.selectProtocolsToEnable', '请选择要启用的协议：');
        protocolsTitle.style.cssText = `
            font-size: 14px; color: var(--FG-1); margin-bottom: 12px;
            font-weight: 500;
        `;
        protocolsContainer.appendChild(protocolsTitle);
        
        // 创建复选框
        const checkboxes = {};
        protocols.forEach(protocol => {
            const checkboxDiv = document.createElement('div');
            checkboxDiv.style.cssText = `
                display: flex; align-items: center; margin-bottom: 12px;
                padding: 10px; border-radius: 4px;
                background: ${protocol.required ? 'rgba(43, 110, 192, 0.05)' : 'transparent'};
                border: ${protocol.required ? '1px solid rgba(43, 110, 192, 0.2)' : '1px solid transparent'};
            `;
            
            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.id = `protocol_${protocol.name}`;
            checkbox.checked = true; // 默认全选
            checkbox.disabled = protocol.required === true; // 必选项禁用复选框（强制选中）
            checkbox.style.cssText = `margin-right: 10px;`;
            checkboxes[protocol.name] = checkbox;
            
            const label = document.createElement('label');
            label.htmlFor = checkbox.id;
            
            // 构建标签内容
            let labelContent = `<strong>${protocol.name}</strong>`;
            if (protocol.required) {
                labelContent += ` <span style="color: #e74c3c; font-size: 12px;">${tr('common.required', '（必选）')}</span>`;
            } else {
                labelContent += ` <span style="color: #95a5a6; font-size: 12px;">${tr('common.optional', '（可选）')}</span>`;
            }
            labelContent += `<br><span style="font-size: 12px; color: var(--FG-3);">${protocol.description}</span>`;
            
            label.innerHTML = labelContent;
            label.style.cssText = `
                font-size: 13px; color: var(--FG-1); cursor: pointer; flex: 1;
            `;
            
            checkboxDiv.appendChild(checkbox);
            checkboxDiv.appendChild(label);
            protocolsContainer.appendChild(checkboxDiv);
        });
        
        dialog.appendChild(protocolsContainer);
        
        // 按钮容器
        const buttonContainer = document.createElement('div');
        buttonContainer.style.cssText = `
            display: flex; gap: 10px; justify-content: flex-end;
        `;
        
        // 取消按钮
        const cancelBtn = document.createElement('button');
        cancelBtn.textContent = tr('common.cancel', '取消');
        cancelBtn.className = 'normal-btn';
        cancelBtn.onclick = () => {
            document.body.removeChild(dialog);
            document.body.removeChild(overlay);
            if (onCancel) onCancel();
            resolve(null);
        };
        buttonContainer.appendChild(cancelBtn);
        
        // 确认按钮
        const confirmBtn = document.createElement('button');
        confirmBtn.textContent = tr('message.enableSelectedProtocols', '启用选中的协议');
        confirmBtn.className = 'main-btn';
        confirmBtn.onclick = () => {
            // 获取选中的协议
            const selectedProtocols = [];
            protocols.forEach(protocol => {
                const checkbox = checkboxes[protocol.name];
                if (checkbox && checkbox.checked) {
                    selectedProtocols.push(protocol.name);
                }
            });
            
            // 检查必选协议是否被选中
            const requiredProtocols = protocols.filter(p => p.required).map(p => p.name);
            const missingRequired = requiredProtocols.filter(name => !selectedProtocols.includes(name));
            
            if (missingRequired.length > 0) {
                alert(trFormat('message.selectRequiredProtocols',
                    { protocols: missingRequired.join('、') },
                    `请选择必选协议：${missingRequired.join('、')}`));
                return;
            }
            
            if (selectedProtocols.length === 0 && !options.allowEmpty) {
                alert(tr('message.selectAtLeastOneProtocol', '请至少选择一个协议'));
                return;
            }
            
            document.body.removeChild(dialog);
            document.body.removeChild(overlay);
            if (onConfirm) onConfirm(selectedProtocols);
            resolve(selectedProtocols);
        };
        buttonContainer.appendChild(confirmBtn);
        
        dialog.appendChild(buttonContainer);
        
        // 添加到页面
        document.body.appendChild(overlay);
        document.body.appendChild(dialog);
        
        // ESC键关闭
        const escHandler = (e) => {
            if (e.key === 'Escape') {
                cancelBtn.click();
                document.removeEventListener('keydown', escHandler);
            }
        };
        document.addEventListener('keydown', escHandler);
    });
}

// 注释：此函数已废弃，新的检查逻辑在workModeSubmit中实现
// 检查逻辑已整合到提交时进行，并支持自动启用协议功能
// 保留函数定义以避免调用错误，但不执行任何操作
async function checkProtocolStatus() {
    // 此函数已被新的自动调整逻辑取代
    console.log('checkProtocolStatus: 此函数已废弃，检查逻辑已移至提交时');
}

// 检查TCP透传模式的协议配置
async function checkProtocolsForTcpTransparentMode() {
    try {
        const tcpInfo = await fetchData('/tcp_info').catch(() => null);
        const tcpEnabled = tcpInfo && tcpInfo.use_tcp === '1';
        
        if (!tcpEnabled) {
            return {
                valid: false,
                message: 'TCP透传模式需要启用TCP协议',
                unconfiguredProtocols: ['TCP'],
                tcpEnabled: tcpEnabled
            };
        }
        
        return {
            valid: true,
            message: '协议配置检查通过'
        };
    } catch (error) {
        console.error('协议配置检查失败:', error);
        return {
            valid: false,
            message: '协议配置检查失败，请稍后重试',
            unconfiguredProtocols: []
        };
    }
}

// 滚动到协议管理区域
function scrollToProtocolSection() {
    // 根据实际页面结构调整，这里假设协议管理区域有相应的ID或类名
    const protocolSections = [
        document.getElementById('mqttConfig'),
        document.getElementById('tcpConfig'), 
        document.getElementById('httpConfig'),
        document.querySelector('.mqtt-config'),
        document.querySelector('.tcp-config'),
        document.querySelector('.http-config')
    ];
    
    // 找到第一个存在的协议配置区域并滚动到那里
    for (const section of protocolSections) {
        if (section) {
            section.scrollIntoView({ behavior: 'smooth', block: 'start' });
            // 可以添加高亮效果
            section.style.outline = '2px solid #2b6ec0';
            setTimeout(() => {
                section.style.outline = '';
            }, 3000);
            break;
        }
    }
}
