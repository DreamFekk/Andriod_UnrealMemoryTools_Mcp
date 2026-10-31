'use strict';
const $ = (id) => document.getElementById(id);
const saved = (key, fallback) => { try { return localStorage.getItem(key) || fallback; } catch { return fallback; } };
const save = (key, value) => { try { localStorage.setItem(key, value); } catch { /* Private browsing. */ } };
let language = saved('umt-language', 'zh');
let theme = saved('umt-theme', matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
let state = {}, processes = [], files = [], online = false, pending = false, activeTab = 'offsets';
let detailSignature = '', fileSignature = '', lastCompleted = '', lastTaskError = '';
const strings = {
  zh: { connecting:'连接中', online:'设备已连接', offline:'连接已断开', workspace:'DEVICE WORKSPACE', title:'内存分析，一步到位。', subtitle:'选择进程，探测结构，导出需要的数据。', target:'目标进程', refresh:'↻ 刷新', loading:'正在加载进程…', noProcesses:'未找到进程，请启动目标应用后刷新', selectTarget:'选择目标进程', processId:'进程 ID', profile:'配置', dedicated:'专用配置', generic:'通用配置', probe:'探测偏移', probeHint:'定位引擎与运行时结构', dump:'导出 SDK', dumpHint:'对象、类型与结构信息', library:'导出 libUE4', libraryHint:'保存运行中的引擎镜像', firstProbe:'先探测偏移，再导出 SDK 或引擎镜像。', available:'偏移已就绪，可以导出 SDK 或引擎镜像。', busyNote:'任务进行中，完成后可开始下一项操作。', activity:'任务状态', idle:'就绪', working:'运行中', complete:'已完成', failed:'失败', ready:'等待选择目标', readyTarget:'可以开始探测', objects:'对象扫描', export:'数据导出', files:'导出文件', noFiles:'导出完成后，在这里下载文件。', download:'下载', offsets:'偏移', structures:'结构', logs:'日志', clearLogs:'清空', search:'搜索名称、类型或偏移…', name:'名称', address:'地址', relative:'模块偏移', status:'状态', found:'已找到', missing:'未找到', noOffsets:'探测结果将在这里显示。', noStructures:'探测后查看引擎结构与字段。', noLogs:'还没有任务日志。', noMatches:'没有匹配的结果。', field:'字段', type:'类型', offset:'偏移', description:'说明', footer:'同一工作区 · 手机与电脑实时同步', switchTheme:'切换主题', requestFailed:'请求失败', timeout:'设备响应超时', connectionError:'无法连接设备，正在自动重试。', duration:'耗时', objectsCount:'对象数', classes:'类型数', bytesRead:'读取量', readCalls:'读取次数', throughput:'读取速度', elapsedMs:'耗时', elapsedSeconds:'耗时', readBytes:'读取量', requests:'读取次数', cacheHits:'缓存命中', objectCount:'对象数', classCount:'类型数', fieldCount:'字段数', outputBytes:'输出大小', mbPerSecond:'读取速度', probeMs:'探测耗时', dumpMs:'SDK 耗时', libraryMs:'镜像耗时', libraryBytes:'镜像大小', libraryReadCalls:'镜像读取次数', libraryMiBps:'镜像读取速度' },
  en: { connecting:'Connecting', online:'Device connected', offline:'Disconnected', workspace:'DEVICE WORKSPACE', title:'Memory, made clear.', subtitle:'Select a process. Inspect structures. Export what matters.', target:'Target process', refresh:'↻ Refresh', loading:'Loading processes…', noProcesses:'No processes found. Start the app and refresh.', selectTarget:'Select a target process', processId:'Process ID', profile:'Profile', dedicated:'Dedicated', generic:'Generic', probe:'Discover offsets', probeHint:'Locate the engine and runtime structures', dump:'Export SDK', dumpHint:'Objects, types, and structure definitions', library:'Export libUE4', libraryHint:'Save the running engine image', firstProbe:'Discover offsets before exporting the SDK or engine image.', available:'Offsets are ready. Export the SDK or engine image.', busyNote:'A task is running. The next action will be available when it finishes.', activity:'Task activity', idle:'Ready', working:'Running', complete:'Complete', failed:'Failed', ready:'Waiting for a target', readyTarget:'Ready to discover offsets', objects:'Object scan', export:'Data export', files:'Exported files', noFiles:'Completed exports will appear here for download.', download:'Download', offsets:'Offsets', structures:'Structures', logs:'Logs', clearLogs:'Clear', search:'Search names, types, or offsets…', name:'Name', address:'Address', relative:'Module offset', status:'Status', found:'Found', missing:'Not found', noOffsets:'Discovered offsets will appear here.', noStructures:'Inspect engine structures after discovering offsets.', noLogs:'No task logs yet.', noMatches:'No matching results.', field:'Field', type:'Type', offset:'Offset', description:'Description', footer:'One workspace · Live on your phone and desktop', switchTheme:'Switch theme', requestFailed:'Request failed', timeout:'The device response timed out', connectionError:'Cannot reach the device. Retrying automatically.', duration:'Duration', objectsCount:'Objects', classes:'Types', bytesRead:'Bytes read', readCalls:'Read calls', throughput:'Read speed', elapsedMs:'Duration', elapsedSeconds:'Duration', readBytes:'Bytes read', requests:'Read calls', cacheHits:'Cache hits', objectCount:'Objects', classCount:'Types', fieldCount:'Fields', outputBytes:'Output size', mbPerSecond:'Read speed', probeMs:'Probe time', dumpMs:'SDK time', libraryMs:'Image time', libraryBytes:'Image size', libraryReadCalls:'Image read calls', libraryMiBps:'Image read speed' }
};
const t = (key) => (strings[language] || strings.zh)[key] || key;
const node = (tag, text, className) => { const el = document.createElement(tag); if (text !== undefined) el.textContent = String(text); if (className) el.className = className; return el; };
const numeric = (value) => Number.isFinite(Number(value)) ? Number(value) : 0;
function formatBytes(value) { let n = Math.max(0, numeric(value)), i = 0; const units = ['B','KB','MB','GB']; while(n >= 1024 && i < 3) { n /= 1024; i++; } return `${n.toFixed(i ? 1 : 0)} ${units[i]}`; }
function showError(message) { $('error').textContent = message || ''; $('error').hidden = !message; }
async function api(path, body) {
  const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 12000);
  try {
    const response = await fetch(path, { cache:'no-store', signal:controller.signal, ...(body ? {method:'POST', headers:{'Content-Type':'application/json', 'X-UMT-Client':'1'}, body:JSON.stringify(body)} : {}) });
    const result = await response.json();
    if (!response.ok || result.accepted === false || (body && result.error)) {const error=new Error(result.error || result.message || `${t('requestFailed')} (${response.status})`);error.httpStatus=response.status;throw error;}
    return result;
  } catch(error) { if (error.name === 'AbortError') throw new Error(t('timeout')); throw error; }
  finally { clearTimeout(timer); }
}
function setConnection(connected) {
  online = connected;
  $('connection').className = `connection ${connected ? 'online' : 'offline'}`;
  $('connection').lastElementChild.textContent = t(connected ? 'online' : 'offline');
  updateControls();
}
function updateControls() {
  const selected = numeric($('process').value), busy = !!state.busy || pending;
  const probed = state.probed && selected === numeric(state.pid);
  $('process').disabled = busy;
  $('refresh').disabled = busy || !online;
  $('clear-logs').disabled = pending || !online;
  document.querySelectorAll('[data-action]').forEach((button) => {
    button.disabled = !online || !selected || busy || (button.dataset.action !== 'probe' && !probed);
  });
  $('control-note').textContent = t(busy ? 'busyNote' : probed ? 'available' : 'firstProbe');
  const process = processes.find((item) => numeric(item.pid) === selected);
  $('target-pid').textContent = process?.pid || '—';
  $('target-profile').textContent = process?.profileName || '—';
  $('target-mode').hidden = !process;
  $('target-mode').textContent = t(process?.dedicated ? 'dedicated' : 'generic');
  updateExplorerControls();
}
function renderProcesses() {
  const previous = $('process').value;
  const selected = processes.some((item) => String(item.pid) === previous) ? previous : processes.some((item) => numeric(item.pid) === numeric(state.pid)) ? String(state.pid) : '';
  const placeholder = node('option', t(processes.length ? 'selectTarget' : 'noProcesses')); placeholder.value = '';
  const options = processes.map((item) => { const option = node('option', `${item.package} · ${item.pid}`); option.value = item.pid; return option; });
  $('process').replaceChildren(placeholder, ...options);
  $('process').value = selected || (processes.length === 1 ? String(processes[0].pid) : '');
  updateControls();
}
async function refreshProcesses() { processes = await api('/api/processes'); if (!Array.isArray(processes)) throw new Error(t('requestFailed')); renderProcesses(); }
function progress(prefix, value) { const n = Math.max(0, Math.min(100, numeric(value))); $(`${prefix}-value`).textContent = `${n.toFixed(n % 1 ? 1 : 0)}%`; $(`${prefix}-bar`).style.width = `${n}%`; $(`${prefix}-bar`).setAttribute('aria-valuenow', n); }
function renderMetrics() {
  const metrics = state.metrics || {};
  const preferred = ['probeMs','dumpMs','libraryMs','libraryMiBps','libraryBytes','elapsedMs','objectsCount','bytesRead','readCalls'];
  const entries = Object.entries(metrics).filter(([, value]) => typeof value === 'number' && Number.isFinite(value));
  const items = preferred.filter((key) => typeof metrics[key] === 'number' && Number.isFinite(metrics[key])).map((key) => [key, metrics[key]]).slice(0,6);
  if (!items.length) items.push(...entries.slice(0,6));
  $('metrics').hidden = !items.length;
  $('metrics').replaceChildren(...items.map(([key, value]) => {
    let formatted = value.toLocaleString(language === 'zh' ? 'zh-CN' : 'en-US', { maximumFractionDigits:2 });
    if (/bytes/i.test(key)) formatted = formatBytes(value);
    else if (/Ms$/.test(key)) formatted = `${(value / 1000).toFixed(2)} s`;
    else if (/seconds|duration/i.test(key)) formatted += ' s';
    else if (/MiBps/i.test(key)) formatted += ' MiB/s';
    else if (/mbPerSecond|throughput/i.test(key)) formatted += ' MB/s';
    const element = node('div', undefined, 'metric'); element.append(node('span', t(key)), node('strong', formatted)); return element;
  }));
}
function renderState(force = false) {
  syncExplorerSession();
  updateControls();
  const status = state.busy ? 'working' : state.error ? 'failed' : state.probed ? 'complete' : 'idle';
  $('task-status').textContent = t(status); $('task-status').className = `status-pill ${state.busy ? 'busy' : state.probed ? 'done' : ''}`;
  const phaseZh = {Ready:'等待选择目标', Complete:'任务完成', Failed:'任务失败', probe:'探测偏移', dump:'导出 SDK', library:'导出引擎镜像', 'Initializing memory':'初始化内存读取', 'Probing dedicated profile':'探测专用配置', 'Probing AutoFix':'自动识别引擎结构', 'Executable information':'读取可执行文件信息', Names:'读取名称表', Objects:'读取对象表', 'Engine offsets':'导出引擎偏移', 'Saving SDK':'保存 SDK', 'Dumping library':'导出引擎镜像'};
  $('phase').textContent = (language === 'zh' ? phaseZh[state.phase] : null) || state.phase || t($('process').value ? 'readyTarget' : 'ready');
  progress('objects', state.objectsPercent); progress('dump', state.dumpPercent); renderMetrics();
  if (state.error && state.error !== lastTaskError) showError(state.error);
  lastTaskError = state.error || '';
  const signature = JSON.stringify([state.offsets, state.groups, state.logs, $('search').value, language]);
  if (force || signature !== detailSignature) { detailSignature = signature; renderDetails(); }
}
function matches(values) { const query = $('search').value.trim().toLowerCase(); return !query || values.some((v) => String(v ?? '').toLowerCase().includes(query)); }
function cells(values) { const row = node('tr'); values.forEach((value) => row.append(value instanceof Node ? value : node('td', value ?? '—'))); return row; }
function statusCell(found) { const cell = node('td'); cell.append(node('span', t(found ? 'found' : 'missing'), found ? 'found' : 'missing')); return cell; }
function renderDetails() {
  const offsets = (state.offsets || []).filter((item) => matches([item.name, item.value, item.relative]));
  $('offset-rows').replaceChildren(...offsets.map((item) => cells([item.name, item.value, item.relative, statusCell(item.found)])));
  $('offset-empty').hidden = !!offsets.length;
  $('offset-empty').textContent = t($('search').value ? 'noMatches' : 'noOffsets');
  const openGroups = new Set([...$('groups').querySelectorAll('details[open]')].map((d) => d.dataset.name));
  const groups = (state.groups || []).map((group) => ({...group, fields:(group.fields || []).filter((field) => matches([group.name, field.name, field.type, field.offset, field.description]))})).filter((group) => group.fields.length);
  $('groups').replaceChildren(...groups.map((group) => {
    const details = node('details'); details.dataset.name = group.name; details.open = !!$('search').value || openGroups.has(group.name);
    const summary = node('summary', group.name); summary.append(node('span', group.fields.length, 'count'));
    const wrap = node('div', undefined, 'table-wrap'), table = node('table'), head = node('thead'), header = node('tr'), body = node('tbody');
    ['field','type','offset','description','status'].forEach((key) => header.append(node('th', t(key)))); head.append(header);
    group.fields.forEach((field) => body.append(cells([field.name, field.type, field.offset, node('td', field.description || '—', 'description'), statusCell(field.found)])));
    table.append(head, body); wrap.append(table); details.append(summary, wrap); return details;
  }));
  $('group-empty').hidden = !!groups.length; $('group-empty').textContent = t($('search').value ? 'noMatches' : 'noStructures');
  const log = $('log-lines'), atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 32;
  const lines = (state.logs || []).filter((line) => matches([line]));
  log.textContent = lines.join('\n'); log.hidden = !lines.length;
  $('log-empty').hidden = !!lines.length; $('log-empty').textContent = t($('search').value ? 'noMatches' : 'noLogs');
  if (atBottom) log.scrollTop = log.scrollHeight;
}
function renderFiles(force = false) {
  const signature = JSON.stringify([files, language]); if (!force && fileSignature === signature) return; fileSignature = signature;
  $('file-count').textContent = files.length;
  if (!files.length) { const empty = node('div', undefined, 'empty compact'); empty.append(node('span', '↧', 'empty-symbol'), node('span', t('noFiles'))); $('files').replaceChildren(empty); return; }
  $('files').replaceChildren(...files.map((file) => {
    const row = node('div', undefined, 'file'), info = node('div', undefined, 'file-info');
    info.append(node('div', file.name, 'file-name'), node('div', formatBytes(file.size), 'file-size'));
    row.append(node('span', '↓', 'file-icon'), info);
    try { const url = new URL(file.url, location.href); if (url.origin === location.origin && /^https?:$/.test(url.protocol)) { const link = node('a', t('download'), 'download'); link.href = url.href; link.download = file.name; row.append(link); } } catch { /* Invalid download URLs are not exposed. */ }
    return row;
  }));
}
async function refreshFiles() { const result = await api('/api/files'); if (Array.isArray(result)) { files = result; renderFiles(); } }
function translate() {
  document.documentElement.lang = language === 'zh' ? 'zh-CN' : 'en';
  document.querySelectorAll('[data-i18n]').forEach((element) => { element.textContent = t(element.dataset.i18n); });
  $('language').textContent = language === 'zh' ? 'EN' : '中';
  $('theme').setAttribute('aria-label', t('switchTheme')); $('search').placeholder = t('search'); $('search').setAttribute('aria-label', t('search'));
  renderProcesses(); renderState(true); renderFiles(true); if (online) setConnection(true);
  renderExplorer();
  renderWorkbench();renderWorldBrowser();
}
function applyTheme() { document.documentElement.dataset.theme = theme; $('theme').textContent = theme === 'dark' ? '☀' : '☾'; document.querySelector('meta[name=theme-color]').content = theme === 'dark' ? '#12141b' : '#f4f5fa'; }
async function action(name) {
  pending = true; updateControls(); showError('');
  try { await api('/api/action', {action:name, pid:numeric($('process').value)}); state = await api('/api/state'); renderState(true); if (!state.busy && name !== 'clear_logs') await refreshFiles(); }
  catch(error) { showError(error.message); }
  finally { pending = false; updateControls(); }
}
function selectTab(name) {
  activeTab = name;
  document.querySelectorAll('[data-tab]').forEach((button) => { const selected = button.dataset.tab === name; button.classList.toggle('active', selected); button.setAttribute('aria-selected', selected); button.tabIndex = selected ? 0 : -1; $(`view-${button.dataset.tab}`).hidden = !selected; });
  $('clear-logs').hidden = name !== 'logs';
  $('search').hidden = name === 'explorer' || name === 'world';
  if(name==='world')$('world-inspector-host').append($('shared-object-inspector'));
  else if(name==='explorer')document.querySelector('#view-explorer .explorer-layout').append($('shared-object-inspector'));
  if(name==='world' && !worldBrowser.worlds.length && explorerReady() && !worldBrowser.loading)discoverWorlds();
  if (name === 'explorer' && !explorer.pages.length && explorerReady() && !explorer.loading) searchObjects();
}
async function poll() {
  try {
    const wasOnline = online, wasBusy = state.busy; state = await api('/api/state'); setConnection(true); renderState();
    if (!wasOnline) { showError(state.error || ''); await Promise.all([refreshProcesses(), refreshFiles()]); api('/api/health').then((health) => { if (health.version) $('version').textContent = `UMT / ${health.version}`; }).catch(() => {}); }
    const completed = JSON.stringify([state.resultPath, state.soDumpPath, state.phase]);
    if (!state.busy && (wasBusy || completed !== lastCompleted)) { await refreshFiles(); lastCompleted = completed; }
  } catch(error) { setConnection(false); showError(`${t('connectionError')} ${error.message}`); }
  finally { setTimeout(poll, !online ? 2200 : state.busy ? 500 : 1400); }
}
$('language').addEventListener('click', () => { language = language === 'zh' ? 'en' : 'zh'; save('umt-language', language); translate(); });
$('theme').addEventListener('click', () => { theme = theme === 'dark' ? 'light' : 'dark'; save('umt-theme', theme); applyTheme(); });
$('process').addEventListener('change', () => { renderState(); showError(''); });
$('refresh').addEventListener('click', async () => { pending = true; updateControls(); try { await refreshProcesses(); showError(''); } catch(error) { showError(error.message); } finally { pending = false; updateControls(); } });
$('search').addEventListener('input', () => renderState(true));
$('clear-logs').addEventListener('click', () => action('clear_logs'));
document.querySelectorAll('[data-action]').forEach((button) => button.addEventListener('click', () => action(button.dataset.action)));
document.querySelectorAll('[data-tab]').forEach((button) => {
  button.addEventListener('click', () => selectTab(button.dataset.tab));
  button.addEventListener('keydown', (event) => { const names = ['offsets','structures','explorer','world','logs']; if (!['ArrowLeft','ArrowRight','Home','End'].includes(event.key)) return; event.preventDefault(); const index = event.key === 'Home' ? 0 : event.key === 'End' ? names.length-1 : (names.indexOf(activeTab) + (event.key === 'ArrowRight' ? 1 : names.length-1)) % names.length; selectTab(names[index]); $(`tab-${names[index]}`).focus(); });
});

// SDK explorer keeps addresses as strings throughout: a 64-bit pointer must
// never pass through JavaScript Number (which only preserves 53 integer bits).
const explorer = {generation:null, session:null, pages:[], page:0, query:'', detail:null, detailKind:'object', history:[], tags:new Map(), loading:false, operation:'', controller:null, sequence:0, error:''};
Object.assign(strings.zh, {explorer:'SDK 浏览器', searchObjects:'搜索', pageSize:'每页', stopSearch:'停止搜索', previousPage:'← 上一页', nextPage:'下一页 →', goBack:'← 返回', inspect:'检查', selectObject:'选择对象或输入地址，查看属性、函数与实时数据。', addTag:'☆ 标记', removeTag:'★ 已标记', objectsHint:'按对象名、完整路径或类型搜索', explorerNeedsProbe:'先探测当前目标，再浏览 SDK 对象。', explorerBusy:'探测或导出进行中，浏览器暂时暂停。', explorerReady:'实时对象浏览 · 点击指针继续查看', searchingObjects:'正在扫描对象', noObjects:'没有匹配的对象。', scanned:'已扫描', totalObjects:'总对象', matched:'本页', metadata:'元数据', properties:'属性', functions:'函数', containers:'容器', runtime:'运行时', value:'值', index:'索引', count:'数量', capacity:'容量', elementSize:'元素大小', dataAddress:'数据地址', objectClass:'类对象', outer:'Outer', super:'父类型', flags:'标志', numParams:'参数数', paramSize:'参数大小', signature:'签名', actors:'Actors', noRows:'没有可显示的数据。', invalidAddress:'请输入有效的十六进制地址，例如 0x79A0123450。', objectSearchFailed:'对象读取失败', generationChanged:'目标会话已更新，历史和标记已重置。', removeBookmark:'删除标记', inspectField:'检查 FField', maxRows:'详细行数', searching:'搜索中…', stopped:'扫描已停止，点击下一页继续。'});
Object.assign(strings.en, {explorer:'SDK browser', searchObjects:'Search', pageSize:'Rows', stopSearch:'Stop search', previousPage:'← Previous', nextPage:'Next →', goBack:'← Back', inspect:'Inspect', selectObject:'Select an object or enter an address to inspect live properties, functions, and data.', addTag:'☆ Bookmark', removeTag:'★ Bookmarked', objectsHint:'Object name, full path, or class', explorerNeedsProbe:'Discover offsets for the selected target to browse SDK objects.', explorerBusy:'A probe or export is running. Object browsing is paused.', explorerReady:'Live objects · Follow pointers to inspect related objects', searchingObjects:'Scanning objects', noObjects:'No matching objects.', scanned:'Scanned', totalObjects:'Objects', matched:'On page', metadata:'Metadata', properties:'Properties', functions:'Functions', containers:'Containers', runtime:'Runtime', value:'Value', index:'Index', count:'Count', capacity:'Capacity', elementSize:'Element size', dataAddress:'Data address', objectClass:'Class object', outer:'Outer', super:'Super', flags:'Flags', numParams:'Parameters', paramSize:'Parameter size', signature:'Signature', actors:'Actors', noRows:'No data available.', invalidAddress:'Enter a valid hexadecimal address, such as 0x79A0123450.', objectSearchFailed:'Could not read object', generationChanged:'Target session changed. History and bookmarks were reset.', removeBookmark:'Remove bookmark', inspectField:'Inspect FField', maxRows:'Detail rows', searching:'Searching…', stopped:'Scan stopped. Use Next to continue.'});
const display = (value) => value == null ? '—' : typeof value === 'object' ? JSON.stringify(value) : String(value);
Object.assign(strings.zh,{objectProperties:'对象自身属性',enums:'枚举值'});
Object.assign(strings.en,{objectProperties:'Object properties',enums:'Enum values'});
const validAddress = (value) => typeof value === 'string' && /^0x[0-9a-f]{1,16}$/i.test(value) && !/^0x0+$/i.test(value);
function explorerReady() { return online && !state.busy && !pending && state.probed && numeric($('process').value) === numeric(state.pid); }
function cancelExplorer() { explorer.sequence++; explorer.controller?.abort(); explorer.controller=null; explorer.loading=false; explorer.operation=''; }
function resetExplorer(generation = null) {
  stopLiveInspection();
  cancelExplorer(); Object.assign(explorer,{generation,pages:[],page:0,detail:null,detailKind:'object',history:[],error:''}); explorer.tags.clear();explorer.openStructures?.clear();
  $('inspect-address').value=''; renderExplorer();
}
function syncExplorerSession() {
  const session = `${state.pid || 0}:${state.probed ? 1 : 0}`;
  const changed = explorer.session !== null && explorer.session !== session;
  const generationChanged = state.generation !== undefined && explorer.generation !== null && state.generation !== explorer.generation;
  if (changed || generationChanged){resetExplorer(state.generation ?? null);resetWorldBrowser();}
  explorer.session=session;renderWorldBrowser();
  if (state.busy && explorer.loading) { cancelExplorer(); renderExplorer(); }
}
function acceptGeneration(generation) {
  if (generation === undefined || generation === null) return false;
  const changed = explorer.generation !== null && generation !== explorer.generation;
  if (changed) { explorer.pages=[]; explorer.page=0; explorer.history=[]; explorer.tags.clear(); explorer.detail=null; }
  explorer.generation=generation; return changed;
}
function updateExplorerControls() {
  if (!$('object-search')) return;
  const ready=explorerReady(), page=explorer.pages[explorer.page];
  $('explorer-notice').textContent=t(state.busy ? 'explorerBusy' : ready ? 'explorerReady' : 'explorerNeedsProbe');
  $('object-search').disabled=!ready || explorer.loading;
  $('object-limit').disabled=!ready || explorer.loading;
  $('object-stop').hidden=!explorer.loading || explorer.operation !== 'list';
  $('object-prev').disabled=!ready || explorer.loading || explorer.page===0;
  $('object-next').disabled=!ready || explorer.loading || !page || (explorer.page===explorer.pages.length-1 && page.complete);
  $('inspect-go').disabled=!ready || explorer.loading;
  $('inspect-back').disabled=!ready || explorer.loading || !explorer.history.length;
  $('object-tag').disabled=!explorer.detail;
  document.querySelectorAll('.object-select,.pointer-button,.bookmark-open').forEach((button)=>{button.disabled=!ready || (explorer.loading && explorer.operation !== 'list');});
}
async function explorerRequest(body, sequence) {
  const controller=new AbortController(); explorer.controller=controller;
  const timer=setTimeout(()=>controller.abort(),12000);
  try {
    const response=await fetch('/api/explorer',{method:'POST',headers:{'Content-Type':'application/json','X-UMT-Client':'1'},body:JSON.stringify(body),signal:controller.signal,cache:'no-store'});
    const result=await response.json();
    if (sequence!==explorer.sequence) throw new DOMException('Cancelled','AbortError');
    if (!response.ok || result.error) {const error=new Error(result.error || t('requestFailed')+' ('+response.status+')');error.busy=!!result.busy;throw error;}
    return result;
  } finally { clearTimeout(timer); if(explorer.controller===controller) explorer.controller=null; }
}
async function searchObjects(next = false) {
  if (!explorerReady()) return;
  cancelExplorer();
  if (!next) { explorer.query=$('object-query').value.trim(); explorer.pages=[]; explorer.page=0; }
  const previous=explorer.pages[explorer.page];
  let pageIndex=next ? explorer.page+1 : 0;
  let cursor=next && previous ? previous.nextCursor : 0;
  const page={items:[],nextCursor:cursor,total:0,complete:false,scanned:0};
  const limit=numeric($('object-limit').value), sequence=explorer.sequence;
  explorer.pages[pageIndex]=page; explorer.page=pageIndex; explorer.loading=true; explorer.operation='list'; explorer.error=''; renderExplorer();
  try {
    while (sequence===explorer.sequence && page.items.length<limit && !page.complete) {
      const result=await explorerRequest({action:'list',query:explorer.query,cursor,limit:limit-page.items.length,scanLimit:4096},sequence);
      if (acceptGeneration(result.generation)) {
        pageIndex=0; explorer.page=0; page.items=[];page.scanned=0;
        if(cursor>0) { cursor=0;page.nextCursor=0;explorer.pages[0]=page;continue; }
      }
      page.items.push(...(Array.isArray(result.items)?result.items:[])); page.total=numeric(result.total); page.scanned+=numeric(result.scanned);
      page.nextCursor=numeric(result.nextCursor); page.complete=!!result.complete;
      explorer.pages[pageIndex]=page; renderExplorerList(); updateExplorerControls();
      if (page.nextCursor<=cursor && !page.complete) throw new Error('The object scan cursor did not advance.');
      cursor=page.nextCursor;
      if (!explorer.query || page.complete || page.items.length>=limit) break;
      await new Promise((resolve)=>setTimeout(resolve,20));
    }
  } catch(error) { if(sequence===explorer.sequence) explorer.error=error.name==='AbortError'?t('timeout'):error.message; }
  finally { if(sequence===explorer.sequence) { explorer.loading=false; explorer.operation=''; renderExplorer(); } }
}
async function inspectObject(address, kind='object', back=false, refreshing=false, options={}) {
  if (!explorerReady()) return;
  address=String(address).trim();
  if (!validAddress(address)) { explorer.error=t('invalidAddress'); renderExplorer(); return; }
  if(refreshing)options={typeAddress:explorer.detail?.typeAddress,dereference:!!explorer.detail?.dereferenced};
  if(kind==='struct' && !validAddress(options.typeAddress)){explorer.error=t('needStructType');renderExplorer();return;}
  if(!refreshing)stopLiveInspection();
  cancelExplorer(); const sequence=explorer.sequence;
  explorer.loading=true; explorer.operation='inspect'; explorer.error=''; updateExplorerControls();
  try {
    const guard=refreshing?{...(explorer.generation!==null?{generation:explorer.generation}:{}),...(explorer.detail?.identity?{expectedIdentity:explorer.detail.identity}:{})}:{};
    const result=await explorerRequest({action:'inspect',address,kind,maxRows:numeric($('inspect-limit').value),...options,...guard},sequence);
    const changed=acceptGeneration(result.generation);
    if (!changed && !back && explorer.detail && (explorer.detail.address!==result.address || explorer.detailKind!==kind || explorer.detail.typeAddress!==result.typeAddress)) explorer.history.push({address:explorer.detail.sourceAddress || explorer.detail.address,kind:explorer.detailKind,typeAddress:explorer.detail.typeAddress,dereference:!!explorer.detail.dereferenced});
    if (back) explorer.history.pop();
    if(refreshing && !changed)markChangedValues(explorer.detail,result);
    if(changed || kind==='field')stopLiveInspection();
    explorer.detail=result; explorer.detailKind=kind;
    $('inspect-address').value=result.sourceAddress || result.address || address; $('inspect-kind').value=kind;
    $('inspect-type-address').value=result.typeAddress || '';$('inspect-dereference').checked=!!result.dereferenced;
  } catch(error) { if(sequence===explorer.sequence) {explorer.error=error.name==='AbortError'?t('timeout'):error.message;if(refreshing && !error.busy)stopLiveInspection();} }
  finally { if(sequence===explorer.sequence) {explorer.loading=false; explorer.operation=''; renderExplorer();} }
}
function pointer(address, label, kind='object') {
  if(!validAddress(address)) return node('span',label ?? display(address),'address-text');
  const button=node('button',label ?? address,'pointer-button'); button.type='button'; button.title=address; button.addEventListener('click',()=>inspectObject(address,kind)); return button;
}
function explorerTable(headers, rows, className='') {
  const wrap=node('div',undefined,`table-wrap explorer-table ${className}`), table=node('table'), head=node('thead'), header=node('tr'), body=node('tbody');
  headers.forEach((key)=>header.append(node('th',t(key)))); head.append(header);
  rows.forEach((row)=>body.append(cells(row.map((value)=>value instanceof Node && value.tagName!=='TD' ? (()=>{const td=node('td');td.append(value);return td;})() : value))));
  table.append(head,body);wrap.append(table);return wrap;
}
function section(title, count, content, opened=false) {
  const details=node('details');details.dataset.section=title;details.open=opened;
  const summary=node('summary',t(title));if(count!==undefined) summary.append(node('span',count,'count'));
  details.append(summary,content);return details;
}
function renderExplorerList() {
  const page=explorer.pages[explorer.page];
  $('object-list').replaceChildren(...(page?.items || []).map((item)=>{
    const button=node('button',undefined,'object-select');button.type='button';button.classList.toggle('selected',item.address===explorer.detail?.address);
    button.title=item.fullName || item.name;
    button.append(node('span',item.name || item.fullName,'object-name'),node('span',`${item.className || 'UObject'} · #${item.index}`,'object-type'),node('code',item.address,'object-address'));
    button.addEventListener('click',()=>inspectObject(item.address));return button;
  }));
  if (!page?.items.length) $('object-list').append(node('div',t(explorer.loading?'searching':'noObjects'),'empty'));
  $('object-summary').textContent=page ? `${t('matched')} ${page.items.length} / ${t('totalObjects')} ${page.total.toLocaleString()}` : '';
  $('object-page').textContent=page ? `${explorer.page+1} · ${t('scanned')} ${page.nextCursor.toLocaleString()}` : '—';
  if(explorer.loading && explorer.operation==='list') $('explorer-notice').textContent=`${t('searchingObjects')} · ${page?.nextCursor.toLocaleString() || 0} / ${page?.total.toLocaleString() || '…'}`;
}
function renderExplorerDetail() {
  const detail=explorer.detail;
  $('object-detail').hidden=!detail;$('object-detail-empty').hidden=!!detail;
  if(!detail) return;
  $('inspect-sample').textContent=t('sampleTime')+': '+new Date(detail.sampledAtMs || Date.now()).toLocaleTimeString()+' · '+t('changedValues')+': '+(detail.changedCount || 0);
  const opened=new Set([...$('object-sections').querySelectorAll('details[open]')].map((item)=>item.dataset.section));
  const identity=`${explorer.detailKind}:${detail.address}`;
  const fresh=$('object-title').dataset.address!==identity;
  $('object-title').dataset.address=identity;
  $('object-class').textContent=detail.className || explorer.detailKind;
  $('object-title').textContent=detail.name || detail.address;$('object-fullname').textContent=detail.fullName || '';
  $('object-tag').textContent=t(explorer.tags.has(`${explorer.detailKind}:${detail.address}`)?'removeTag':'addTag');
  $('object-links').replaceChildren(node('code',detail.address),...['classAddress','outerAddress','superAddress'].filter((key)=>validAddress(detail[key])).map((key)=>explorer.detailKind==='field' ? node('span',`${t({classAddress:'objectClass',outerAddress:'outer',superAddress:'super'}[key])}: ${detail[key]}`,'address-text') : pointer(detail[key],t({classAddress:'objectClass',outerAddress:'outer',superAddress:'super'}[key]))));
  if(detail.note) $('object-links').append(node('p',detail.note,'inspector-note'));
  if(detail.isInstance===false){
    $('object-links').append(node('p',t('classDeclarationHint'),'inspector-note'));
    if(validAddress(detail.classDefaultObjectAddress))$('object-links').append(pointer(detail.classDefaultObjectAddress,t('openDefaultObject'),'instance'));
  }
  const meta=explorerTable(['name','value'],(detail.metadata || []).map((item)=>[item.name,display(item.value)]));
  const propertyTable=instancePropertyTable;
  const props=propertyTable(detail.properties);
  const funcs=node('div',undefined,'function-list');
  (detail.functions || []).forEach((item)=>{const card=node('div',undefined,'function-card');card.append(node('small',item.owner || '','value-storage'),validAddress(item.objectAddress)?pointer(item.objectAddress,item.name):node('strong',item.name),node('code',item.signature || item.name,'function-signature'),node('div',`${item.address || '—'} · RVA ${item.rva || '—'} · ${t('flags')} ${display(item.flags)} · ${t('numParams')} ${display(item.numParams)} · ${t('paramSize')} ${display(item.paramSize)}`,'function-meta'));funcs.append(card);});
  if(!detail.functions?.length) funcs.append(node('div',t('noRows'),'empty compact'));
  const containers=node('div');
  (detail.containers || []).forEach((container)=>{
    const box=node('div',undefined,'container-card');box.append(node('h4',`${container.name} · ${container.type || container.kind}`),node('p',`${t('count')}: ${display(container.count)} / ${t('capacity')}: ${display(container.max ?? container.capacity)} · ${t('elementSize')}: ${display(container.elementSize)}`),node('code',`${container.address} → ${container.dataAddress || container.data || '—'}`));
    if(container.note) box.append(node('p',container.note));
    box.append(explorerTable(['index','type','address','value'],(container.items || []).map((item)=>[item.index,item.type,item.address,liveValueCell(item)])));containers.append(box);
  });
  if(!detail.containers?.length) containers.append(node('div',t('noRows'),'empty compact'));
  const runtime=detail.runtime || {}, runtimeBox=node('div',undefined,'runtime-content');
  if(runtime.kind) runtimeBox.append(node('h4',runtime.kind));
  runtimeBox.append(explorerTable(['name','address','value'],(runtime.fields || []).map((item)=>[item.name,item.address,liveValueCell(item)])));
  if(runtime.actorCount!==undefined) runtimeBox.append(node('p',`${t('actors')}: ${display(runtime.actorCount)} / ${display(runtime.actorMax)}`));
  if(runtime.actors?.length) runtimeBox.append(explorerTable(['index','name','address'],runtime.actors.map((actor)=>[actor.index,pointer(actor.address,actor.fullName || actor.name),actor.address])));
  if(runtime.rowMap) runtimeBox.append(explorerTable(['name','value'],Object.entries(runtime.rowMap).map(([key,value])=>[key,display(value)])));
  if(!runtime.fields?.length && !runtime.actors?.length && !runtime.rowMap) runtimeBox.append(node('div',t('noRows'),'empty compact'));
  const sections=[section('metadata',detail.metadata?.length || 0,meta,fresh || opened.has('metadata')),section('properties',detail.properties?.length || 0,props,fresh || opened.has('properties'))];
  if(detail.classHierarchy?.length)sections.unshift(section('inheritance',detail.classHierarchy.length,explorerTable(['name','address','elementSize'],detail.classHierarchy.map(item=>[pointer(item.address,item.name),item.address,item.size])),opened.has('inheritance')));
  if(detail.objectProperties?.length) sections.push(section('objectProperties',detail.objectProperties.length,propertyTable(detail.objectProperties),opened.has('objectProperties')));
  if(detail.enums?.length) sections.push(section('enums',detail.enums.length,explorerTable(['name','value'],detail.enums.map((item)=>[item.name,display(item.value)])),true));
  sections.push(section('functions',detail.functions?.length || 0,funcs,opened.has('functions')),section('containers',detail.containers?.length || 0,containers,opened.has('containers')),section('runtime',undefined,runtimeBox,opened.has('runtime')));
  $('object-sections').replaceChildren(...sections);
}
function renderExplorerTags() {
  $('object-tags').replaceChildren(...[...explorer.tags.entries()].map(([key,tag])=>{
    const chip=node('span',undefined,'bookmark');const open=node('button',tag.name || tag.address,'bookmark-open');open.type='button';open.title=tag.address;open.addEventListener('click',()=>inspectObject(tag.address,tag.kind,false,false,{typeAddress:tag.typeAddress,dereference:tag.dereference}));
    const remove=node('button','×','bookmark-remove');remove.type='button';remove.setAttribute('aria-label',`${t('removeBookmark')}: ${tag.name || tag.address}`);remove.addEventListener('click',()=>{explorer.tags.delete(key);renderExplorer();});chip.append(open,remove);return chip;
  }));
  $('object-tags').hidden=!explorer.tags.size;
}
function renderExplorer() {
  $('object-query').placeholder=t('objectsHint');$('object-query').setAttribute('aria-label',t('objectsHint'));
  $('explorer-error').hidden=!explorer.error;$('explorer-error').textContent=explorer.error;
  renderExplorerList();renderExplorerDetail();renderExplorerTags();updateExplorerControls();
  $('inspect-refresh').disabled=!explorerReady() || explorer.loading || !explorer.detail;
  $('inspect-live').disabled=!explorer.detail || explorer.detailKind==='field';
}
$('object-search-form').addEventListener('submit',(event)=>{event.preventDefault();searchObjects();});
$('object-limit').addEventListener('change',()=>searchObjects());
$('object-stop').addEventListener('click',()=>{cancelExplorer();renderExplorer();$('explorer-notice').textContent=t('stopped');});
$('object-prev').addEventListener('click',()=>{if(explorer.page>0){explorer.page--;renderExplorer();}});
$('object-next').addEventListener('click',()=>{if(explorer.page+1<explorer.pages.length){explorer.page++;renderExplorer();}else searchObjects(true);});
$('inspect-form').addEventListener('submit',(event)=>{event.preventDefault();inspectObject($('inspect-address').value,$('inspect-kind').value,false,false,{...($('inspect-kind').value==='struct'?{typeAddress:$('inspect-type-address').value.trim()}:{}),dereference:$('inspect-dereference').checked});});
$('inspect-limit').addEventListener('change',()=>{if(explorer.detail) inspectObject(explorer.detail.sourceAddress || explorer.detail.address,explorer.detailKind,false,true);});
$('inspect-back').addEventListener('click',()=>{const previous=explorer.history.at(-1);if(previous) inspectObject(previous.address,previous.kind,true,false,{typeAddress:previous.typeAddress,dereference:previous.dereference});});
$('object-tag').addEventListener('click',()=>{const item=explorer.detail;if(!item)return;const key=`${explorer.detailKind}:${item.address}`;if(explorer.tags.has(key))explorer.tags.delete(key);else explorer.tags.set(key,{address:item.sourceAddress || item.address,name:item.name,kind:explorer.detailKind,typeAddress:item.typeAddress,dereference:item.dereferenced});renderExplorer();});

const workbench={status:null,available:false,connected:false,refreshing:false,reconnectTimer:null,connectionIssue:'connecting',loading:false,activeCommand:null,jobStartedAt:0,idaServiceMessage:'',functions:[],selected:null,result:null,job:null,timer:null,error:''};
Object.assign(strings.zh,{workbench:'分析工作台',workbenchUnavailable:'电脑端启动器提供 IDA 与 MCP 分析工作台。手机仍可使用上方完整 SDK 浏览器。',dumpSessions:'Dump 会话',symbolsOnly:'只同步符号（不下载库）',syncDump:'同步手机 Dump',checkIda:'检查 IDA 连接',openIda:'在 IDA 打开库',findFunction:'查找函数',analyzeFunction:'分析所选函数',analysisResult:'分析结果',analysisEmpty:'同步 Dump，选择函数，再获取 IDA 反编译与引用证据。',decompile:'反编译',xrefs:'交叉引用',rawEvidence:'原始证据',agentTask:'Agent 任务',agentTaskHint:'将目标与函数证据组合成任务，交给已连接 MCP 的 Agent。',agentGoal:'希望实现的目标',buildTask:'生成任务',copyTask:'复制任务',agentTruth:'此处生成任务与证据；模型执行由所连接的 Agent 完成。',mcpConnect:'连接 MCP 与 IDA',mcpHint:'将 bridge/mcp_config.json 加入 Agent 的 MCP 配置。保持本机 IDA MCP 服务运行；工作台通过本机 bridge 调用。',copyConfig:'复制 MCP 配置',downloadReport:'下载报告',idaConnected:'IDA 已连接',idaDisconnected:'IDA 未连接',noSessions:'尚未同步 Dump',noFunctions:'搜索结果将在这里显示。',functionHint:'函数名称 / RVA',jobRunning:'任务运行中',jobDone:'任务完成',jobFailed:'任务失败',copied:'已复制',copyManually:'请选择文本手动复制',noDecompile:'当前结果没有反编译文本，请查看原始证据。',noXrefs:'没有交叉引用数据。',agentGoalHint:'例如：分析此函数的参数与调用关系，解释其行为并提供等价实现。',enterGoal:'请先填写目标。'});
Object.assign(strings.en,{workbench:'Analysis workbench',workbenchUnavailable:'Launch the desktop bridge for IDA and MCP tools. The full SDK browser above remains available on your phone.',dumpSessions:'Dump sessions',symbolsOnly:'Symbols only (skip library download)',syncDump:'Sync phone dump',checkIda:'Check IDA connection',openIda:'Open library in IDA',findFunction:'Find a function',analyzeFunction:'Analyze selected function',analysisResult:'Analysis result',analysisEmpty:'Sync a dump, select a function, then collect IDA decompilation and reference evidence.',decompile:'Decompilation',xrefs:'Cross references',rawEvidence:'Raw evidence',agentTask:'Agent task',agentTaskHint:'Combine your objective and function evidence for an Agent connected through MCP.',agentGoal:'What should the Agent accomplish?',buildTask:'Build task',copyTask:'Copy task',agentTruth:'This prepares a task and evidence. Your connected Agent performs the model execution.',mcpConnect:'Connect MCP and IDA',mcpHint:'Add bridge/mcp_config.json to your Agent MCP settings. Keep the local IDA MCP service running; the bridge handles local calls.',copyConfig:'Copy MCP config',downloadReport:'Download report',idaConnected:'IDA connected',idaDisconnected:'IDA disconnected',noSessions:'No dump synced yet',noFunctions:'Function search results will appear here.',functionHint:'Function name / RVA',jobRunning:'Task running',jobDone:'Task complete',jobFailed:'Task failed',copied:'Copied',copyManually:'Select the text and copy it manually',noDecompile:'No decompilation text in this result. Inspect the raw evidence.',noXrefs:'No cross-reference data.',agentGoalHint:'For example: analyze parameters and callers, explain the behavior, and provide an equivalent implementation.',enterGoal:'Enter an objective first.'});
Object.assign(strings.zh,{openDesktopWorkbench:'打开电脑工作台（18766）',workbenchConnecting:'正在连接电脑分析工作台…',workbenchUnsupported:'当前打开的是手机服务 / ADB 转发入口，它没有电脑端 IDA/MCP 接口。已有 Dump 和报告没有删除，请在电脑打开 18766 工作台。',workbenchReconnect:'电脑工作台暂时未连接，正在每 4 秒重试。已有结果会保留；若服务未启动，在电脑运行 desktop/start.cmd。',workbenchLaunchHint:'电脑端双击 desktop/start.cmd；手机服务 / ADB 转发端口 18765 不包含 IDA 工作台。自定义端口请使用启动器输出的 Computer 地址。'});
Object.assign(strings.en,{openDesktopWorkbench:'Open desktop workbench (18766)',workbenchConnecting:'Connecting to the desktop analysis workbench…',workbenchUnsupported:'This device / ADB-forwarded endpoint has no desktop IDA/MCP API. Your dumps and reports were not deleted. Open port 18766 on the computer.',workbenchReconnect:'Desktop bridge unavailable. Retrying every 4 seconds and preserving displayed results. Run desktop/start.cmd on the computer if it is stopped.',workbenchLaunchHint:'On the computer, run desktop/start.cmd. Device / ADB port 18765 does not host IDA. For custom ports, use the Computer URL printed by the launcher.'});
function selectedSession(){return $('dump-session').value;}
Object.assign(strings.zh,{loadAnalysis:'查看记录',noAnalysisJobs:'暂无任务记录'});
Object.assign(strings.en,{loadAnalysis:'View job',noAnalysisJobs:'No recent jobs'});
function sessionIdentifier(session){return String(session.id ?? session.session ?? session.name ?? session.path ?? '');}
function renderWorkbench(){
  $('workbench-unavailable').hidden=workbench.connected;$('workbench-content').hidden=!workbench.available;
  $('workbench-connection-message').textContent=t(workbench.connectionIssue==='unsupported'?'workbenchUnsupported':workbench.connectionIssue==='connecting'?'workbenchConnecting':'workbenchReconnect');
  $('workbench-current-origin').textContent=location.origin;
  $('workbench-error').hidden=!workbench.error;$('workbench-error').textContent=workbench.error;
  const startingIda=workbench.loading && workbench.activeCommand==='start_ida';
  $('ida-endpoint-save').disabled=workbench.loading || !workbench.connected;
  $('ida-endpoint-save').textContent=t(startingIda?'startingIda':'startIda');
  $('ida-endpoint-form').setAttribute('aria-busy',String(startingIda));
  $('ida-endpoint').disabled=workbench.loading;
  $('ida-service-message').hidden=!startingIda && !workbench.idaServiceMessage;
  $('ida-service-message').textContent=startingIda?t('startingIda'):workbench.idaServiceMessage;
  $('function-query').placeholder=t('functionHint');$('agent-goal').placeholder=t('agentGoalHint');
  if(!workbench.available)return;
  const status=workbench.status || {}, previous=selectedSession(), sessions=status.sessions || [];
  const previousJob=$('analysis-history').value,jobs=[...(status.jobs || [])].reverse();
  const emptyJob=node('option',t('noAnalysisJobs'));emptyJob.value='';
  $('analysis-history').replaceChildren(...(jobs.length?jobs.map((job)=>{const option=node('option',`${job.kind || 'analysis'} · ${job.status} · ${String(job.id).slice(0,8)}`);option.value=job.id;return option;}):[emptyJob]));
  if(jobs.some((job)=>job.id===previousJob))$('analysis-history').value=previousJob;
  $('load-analysis').disabled=workbench.loading || !workbench.connected || !jobs.length;
  const ida=status.ida || {};const idaLabel=!ida.connected?'idaDisconnected':ida.canOpenDatabase===true?'idaCanOpen':ida.canOpenDatabase===false?'idaCurrentOnly':'idaConnected';$('ida-status').textContent=t(idaLabel);$('ida-status').className='status-pill '+(ida.connected && ida.canOpenDatabase!==false?'done':'');
  $('ida-capability-message').textContent=[ida.error,ida.message].filter((message,index,all)=>message && all.indexOf(message)===index).join('\n');if(!startingIda && document.activeElement!==$('ida-endpoint') && status.idaUrl)$('ida-endpoint').value=status.idaUrl;
  $('workbench-device').textContent=status.deviceUrl || '';$('workbench-ida').textContent=status.idaUrl || '';$('artifact-root').textContent=status.artifactRoot || '';
  const emptySession=node('option',t('noSessions'));emptySession.value='';
  $('dump-session').replaceChildren(...(sessions.length?sessions.map((session)=>{const label=session.name || [session.package || session.device?.package,session.createdUtc].filter(Boolean).join(' · ') || sessionIdentifier(session);const option=node('option',(session.review?.status==='invalid'?'[INVALID] ':'')+label);option.value=sessionIdentifier(session);return option;}):[emptySession]));
  if(sessions.some((session)=>sessionIdentifier(session)===previous))$('dump-session').value=previous;
  $('dump-session').disabled=workbench.loading || !sessions.length;
  ['sync-dump','connect-ida','find-functions','open-ida','analyze-function'].forEach((id)=>{$(id).disabled=workbench.loading || !workbench.connected || (id==='analyze-function' && !workbench.selected) || (['open-ida','find-functions'].includes(id) && !sessions.length);});
  if(status.ida?.canOpenDatabase===false)$('open-ida').disabled=true;
  $('function-results').replaceChildren(...workbench.functions.map((item)=>{const button=node('button',undefined,'function-choice');button.type='button';button.classList.toggle('selected',item===workbench.selected);button.append(node('strong',item.name),node('code',`RVA ${item.rva || '—'}`));button.addEventListener('click',()=>{workbench.selected=item;renderWorkbench();});return button;}));
  if(!workbench.functions.length)$('function-results').append(node('div',t('noFunctions'),'empty compact'));
  $('function-selection').hidden=!workbench.selected;
  if(workbench.selected){$('selected-function-name').textContent=workbench.selected.name;$('selected-function-rva').textContent=`RVA ${workbench.selected.rva || '—'}`;$('selected-function-signature').textContent=workbench.selected.signature || workbench.selected.sdkSnippet || '';}
  $('workbench-job').textContent=workbench.loading?t('jobRunning'):workbench.job?t(workbench.job.status==='error'?'jobFailed':'jobDone'):'';
  const result=workbench.result;$('analysis-empty').hidden=!!result;$('analysis-result').hidden=!result;
  if(result){
    $('agent-report-result').hidden=!result.markdown;
    $('agent-report-markdown').textContent=result.markdown || '';
    const analysis=result.analysis || result.ida || result;
    const recovered=evidenceValue(analysis.evidence?.decompile?.result);
    const decompiled=analysis.decompile ?? analysis.decompiled ?? analysis.pseudocode ?? analysis.decompilation ?? recovered?.code ?? recovered?.pseudocode ?? recovered?.decompiled ?? (typeof recovered==='string'?recovered:'');
    $('analysis-decompile').textContent=typeof decompiled==='string'?decompiled:JSON.stringify(decompiled,null,2);
    if(!$('analysis-decompile').textContent)$('analysis-decompile').textContent=t('noDecompile');
    const xrefEvidence=analysis.xrefs ?? analysis.references ?? evidenceValue(analysis.evidence?.xrefs?.result);
    const xrefs=referenceRows(xrefEvidence);
    $('analysis-xrefs').replaceChildren();
    if(Array.isArray(xrefs) && xrefs.length)$('analysis-xrefs').append(explorerTable(['address','name','type'],xrefs.map((item)=>[item.address ?? item.from ?? item.to ?? '—',item.name ?? item.function ?? item.functionName ?? '—',item.type ?? item.kind ?? '—'])));
    else $('analysis-xrefs').append(node('pre',typeof xrefs==='string' && xrefs?xrefs:typeof xrefs==='object' && Object.keys(xrefs).length?JSON.stringify(xrefs,null,2):t('noXrefs'),'analysis-code compact'));
    $('analysis-raw').textContent=JSON.stringify(result,null,2);
  }
  const report=$('workbench-report');report.hidden=!workbench.job || workbench.job.status!=='done' || workbench.job.kind==='start_ida';if(!report.hidden)report.href=`/api/workbench/report?id=${encodeURIComponent(workbench.job.id || workbench.job.jobId)}`;
  $('mcp-config').textContent=typeof status.mcpConfig==='string'?status.mcpConfig:status.mcpConfig?JSON.stringify(status.mcpConfig,null,2):JSON.stringify({mcpServers:{"unreal-memory-tools":{command:'python',args:['<PROJECT>/bridge/mcp_server.py','--workspace','<PROJECT>/artifacts']}}},null,2);
}
function evidenceValue(result){
  if(!result)return '';
  if(result.structuredContent)return result.structuredContent;
  const text=typeof result==='string'?result:(result.content || []).filter((item)=>item.type==='text').map((item)=>item.text).join('\n');
  if(!text)return result;
  try{return JSON.parse(text);}catch{return text;}
}
function referenceRows(value){
  if(Array.isArray(value))return value.flatMap((item)=>item && (item.xrefs || item.result || item.references)?referenceRows(item.xrefs || item.result || item.references):[item]);
  if(value && typeof value==='object')return referenceRows(value.xrefs || value.result || value.references || []);
  return value || [];
}
async function refreshWorkbench(silent=false){
  if(workbench.refreshing)return;
  clearTimeout(workbench.reconnectTimer);workbench.refreshing=true;
  try{const status=await api('/api/workbench/status');if(!Array.isArray(status.sessions))throw new Error('Invalid workbench status');workbench.status=status;workbench.available=true;workbench.connected=true;if(!silent)workbench.error='';renderWorkbench();}
  catch(error){workbench.connected=false;workbench.connectionIssue=error.httpStatus===404?'unsupported':'offline';renderWorkbench();}
  finally{workbench.refreshing=false;workbench.reconnectTimer=setTimeout(()=>refreshWorkbench(true),workbench.connected?10000:4000);}
}
async function pollWorkbenchJob(id){
  const startingIda=workbench.activeCommand==='start_ida';
  try{
    const job=await api(`/api/workbench/job?id=${encodeURIComponent(id)}`);workbench.job=job;
    const idaJob=startingIda || job.kind==='start_ida';
    if(idaJob)job.kind='start_ida';
    if(job.status==='running' || job.status==='pending' || job.status==='queued'){
      if(idaJob && Date.now()-workbench.jobStartedAt>120000)throw new Error(t('idaStartupTimeout'));
      renderWorkbench();workbench.timer=setTimeout(()=>pollWorkbenchJob(id),700);return;
    }
    workbench.loading=false;
    if(job.status==='error')workbench.error=job.error || t('jobFailed');
    else if(idaJob)applyIdaStartupResult(job.result || {});
    else{workbench.result=job.result ?? job;if(job.result?.function)workbench.selected=job.result.function;if(job.result?.error)workbench.error=job.result.error;await refreshWorkbench(true);const session=job.result?.session ?? job.result?.id;if(session && (workbench.status?.sessions || []).some((item)=>sessionIdentifier(item)===String(session)))$('dump-session').value=String(session);}
    workbench.activeCommand=null;
    if(idaJob)await refreshWorkbench(true);
    renderWorkbench();
  }catch(error){workbench.loading=false;workbench.activeCommand=null;workbench.error=error.message;if(startingIda)await refreshWorkbench(true);renderWorkbench();}
}
function applyIdaStartupResult(result){
  if(workbench.status){if(result.ida)workbench.status.ida=result.ida;if(result.idaUrl)workbench.status.idaUrl=result.idaUrl;}
  workbench.idaServiceMessage=result.service?.message || (result.ida?.connected?t('idaConnected'):'');
  if(result.error || result.ida?.error)workbench.error=result.error || result.ida.error;
  else if(result.ida?.connected===false)workbench.error=result.ida.message || t('idaDisconnected');
}
async function workbenchCommand(command,extra={}){
  if(workbench.loading)return;
  workbench.loading=true;workbench.activeCommand=command;workbench.jobStartedAt=Date.now();workbench.error='';if(command==='start_ida')workbench.idaServiceMessage='';renderWorkbench();
  try{
    const payload={command,...(selectedSession()?{session:selectedSession()}:{}),...extra};
    const answer=await api('/api/workbench/command',payload);
    if(answer.jobId){workbench.job={id:answer.jobId,kind:command,status:answer.status || 'running'};renderWorkbench();pollWorkbenchJob(answer.jobId);return;}
    if(command==='search_functions')workbench.functions=answer.functions || [];
    else if(command==='start_ida'){applyIdaStartupResult(answer);await refreshWorkbench(true);}
    else if(command==='ida_status' || command==='configure_ida'){if(workbench.status)workbench.status.ida=answer.ida || answer;await refreshWorkbench(true);}
    else workbench.result=answer;
  }catch(error){workbench.error=error.message;if(command==='start_ida')await refreshWorkbench(true);}
  workbench.loading=false;workbench.activeCommand=null;renderWorkbench();
}
async function copyText(text,target){try{await navigator.clipboard.writeText(text);$(target).textContent=t('copied');}catch{$(target).textContent=t('copyManually');$('agent-prompt').focus();$('agent-prompt').select();}}
function buildAgentTask(){
  const goal=$('agent-goal').value.trim();if(!goal){$('agent-copy-status').textContent=t('enterGoal');$('agent-goal').focus();return;}
  const prompt=[`Objective / 目标:\n${goal}`,`Dump session: ${selectedSession() || 'not selected'}\nDevice: ${workbench.status?.deviceUrl || ''}\nArtifact root: ${workbench.status?.artifactRoot || ''}`,`Selected SDK function:\n${JSON.stringify(workbench.selected || {},null,2)}`,`IDA / runtime evidence:\n${JSON.stringify(workbench.result || explorer.detail || {},null,2)}`,'Use the Unreal Memory Tools MCP connection to verify the dump session and function RVA. Base conclusions on retrieved evidence; distinguish facts, assumptions, and missing inputs. Produce the requested implementation and concrete validation. Do not invent decompilation or a successful tool result.'].join('\n\n');
  $('agent-prompt').value=prompt;$('agent-prompt').hidden=false;$('agent-copy-status').textContent='';
}
$('workbench-refresh').addEventListener('click',()=>refreshWorkbench());
$('load-analysis').addEventListener('click',()=>{const id=$('analysis-history').value;if(id){workbench.loading=true;workbench.activeCommand=(workbench.status?.jobs || []).find(job=>job.id===id)?.kind || null;workbench.jobStartedAt=Date.now();workbench.error='';renderWorkbench();pollWorkbenchJob(id);}});
$('sync-dump').addEventListener('click',()=>workbenchCommand('sync_dump',{symbolsOnly:$('symbols-only').checked}));
$('connect-ida').addEventListener('click',()=>workbenchCommand('ida_status'));
$('open-ida').addEventListener('click',()=>workbenchCommand('open_in_ida'));
$('function-search-form').addEventListener('submit',(event)=>{event.preventDefault();workbenchCommand('search_functions',{query:$('function-query').value.trim(),limit:100});});
$('analyze-function').addEventListener('click',()=>{if(workbench.selected)workbenchCommand('analyze_function',{name:workbench.selected.name,rva:workbench.selected.rva});});
$('dump-session').addEventListener('change',()=>{workbench.selected=null;workbench.functions=[];workbench.result=null;renderWorkbench();});
$('build-agent-task').addEventListener('click',buildAgentTask);
$('copy-agent-task').addEventListener('click',()=>{if($('agent-prompt').hidden)buildAgentTask();if(!$('agent-prompt').hidden)copyText($('agent-prompt').value,'agent-copy-status');});
$('copy-mcp-config').addEventListener('click',()=>copyText($('mcp-config').textContent,'agent-copy-status'));
Object.assign(strings.zh,{instanceInspector:'实例地址查询 / 实时值',refreshValues:'刷新数值',liveValues:'实时刷新',owner:'所属 Class',nestedFields:'结构成员',sampleTime:'采样',changedValues:'变化成员',inheritance:'Class 继承链',partialFields:'部分成员已省略（深度/数量/时间限制）'});
Object.assign(strings.en,{instanceInspector:'Instance address / live values',refreshValues:'Refresh values',liveValues:'Live refresh',owner:'Declaring class',nestedFields:'Struct members',sampleTime:'Sample',changedValues:'Changed fields',inheritance:'Class hierarchy',partialFields:'Some fields omitted (depth/count/time limit)'});
let liveTimer=null,liveActive=false;
function stopLiveInspection(){clearTimeout(liveTimer);liveTimer=null;liveActive=false;$('inspect-live').checked=false;}
function scheduleLiveInspection(){
  clearTimeout(liveTimer);
  if(!liveActive)return;
  liveTimer=setTimeout(async()=>{
    if(!liveActive)return;
    if(!document.hidden && (activeTab==='explorer' || activeTab==='world') && explorerReady() && !explorer.loading && explorer.detail && explorer.detailKind!=='field')
      await inspectObject(explorer.detail.sourceAddress || explorer.detail.address,explorer.detailKind,false,true);
    if(liveActive)scheduleLiveInspection();
  },Math.max(500,Number($('inspect-interval').value)||1000));
}
function markChangedValues(previous,current){
  const old=new Map();
  function visit(rows,fn){for(const row of rows || []){fn(row);visit(row.children,fn);}}
  visit(previous?.properties,(row)=>old.set(row.address+'|'+row.name,row.value));
  let changed=0;
  visit(current.properties,(row)=>{const key=row.address+'|'+row.name;row.changed=old.has(key)&&old.get(key)!==row.value;if(row.changed)changed++;});
  current.changedCount=changed;
}
function liveValueCell(item){
  const cell=node('td',undefined,'property-value'+(item.changed?' value-changed':''));
  const declaration=item.valueKind==='declaration' || item.value==='<field descriptor>';
  cell.append(declaration?node('span',t('declarationOnly')):validAddress(item.pointer)?pointer(item.pointer,display(item.value)):node('span',display(item.value)));
  if(declaration && item.typeLinks?.length){
    for(const link of item.typeLinks){
      if(!validAddress(link.address))continue;
      const button=pointer(link.address,t('openDeclaredType')+' '+link.name);button.classList.add('declared-type-open');cell.append(button);
    }
  }
  if(item.scalarType)cell.append(node('small',item.scalarType,'value-storage'));
  if(validAddress(item.structAddress) && validAddress(item.address)){
    const open=node('button',t('followStruct'),'pointer-button struct-open');open.type='button';
    open.addEventListener('click',()=>inspectObject(item.address,'struct',false,false,{typeAddress:item.structAddress}));
    cell.append(open);
  }
  if(item.children?.length){
    const nested=node('details',undefined,'nested-struct');
    nested.open=explorer.openStructures?.has(item.address)||false;
    nested.addEventListener('toggle',()=>{if(!nested.isConnected)return;explorer.openStructures ??= new Set();if(nested.open)explorer.openStructures.add(item.address);else explorer.openStructures.delete(item.address);});
    nested.append(node('summary',t('nestedFields')),instancePropertyTable(item.children));cell.append(nested);
  }
  if(item.childrenTruncated)cell.append(node('small',t('partialFields'),'value-storage'));
  if(item.note)cell.append(node('small',item.note,'value-storage'));
  return cell;
}
function instancePropertyTable(items){
  return explorerTable(['name','type','owner','offset','address','value'],(items || []).map((item)=>[
    propertyNameCell(item),
    referencedTypeCell(item),item.owner || '—',item.offset,item.address,liveValueCell(item)]));
}
function propertyNameCell(item){
  const links=(item.typeLinks || []).filter(link=>validAddress(link.address));
  const declaration=item.valueKind==='declaration' || item.value==='<field descriptor>';
  let primary=null;
  if(validAddress(item.pointer))primary=pointer(item.pointer,item.name);
  else if(!declaration && validAddress(item.structAddress) && validAddress(item.address)){
    primary=node('button',item.name,'pointer-button');primary.type='button';
    primary.addEventListener('click',()=>inspectObject(item.address,'struct',false,false,{typeAddress:item.structAddress}));
  }else if(links.length===1){primary=pointer(links[0].address,item.name);primary.title=t('openDeclaredType')+' '+links[0].name;}
  if(!primary)return validAddress(item.descriptorAddress)?pointer(item.descriptorAddress,item.name,item.descriptorKind || 'object'):item.name;
  const cell=node('td');cell.append(primary);
  if(validAddress(item.descriptorAddress)){
    const metadata=pointer(item.descriptorAddress,t('fieldMetadata'),item.descriptorKind || 'object');metadata.classList.add('field-metadata-link');cell.append(metadata);
  }
  return cell;
}
function referencedTypeCell(item){
  const links=(item.typeLinks || []).filter(link=>validAddress(link.address));
  if(!links.length)return item.type;
  const cell=node('td',undefined,'referenced-type');
  if(links.length===1){
    const button=pointer(links[0].address,item.type);button.classList.add('type-link');button.title=t('openDeclaredType')+' '+links[0].name+' @ '+links[0].address;cell.append(button);
  }else{
    cell.append(node('code',item.type));
    for(const link of links){const button=pointer(link.address,link.name);button.classList.add('type-link');cell.append(button);}
  }
  return cell;
}
$('inspect-refresh').addEventListener('click',()=>{if(explorer.detail)inspectObject(explorer.detail.sourceAddress || explorer.detail.address,explorer.detailKind,false,true);});
$('inspect-live').addEventListener('change',()=>{liveActive=$('inspect-live').checked;if(liveActive)scheduleLiveInspection();else stopLiveInspection();});
$('inspect-interval').addEventListener('change',()=>{if(liveActive)scheduleLiveInspection();});
Object.assign(strings.zh,{followStruct:'→ 展开此结构',dereferencePointer:'输入的是指针槽，先解引用一次',needStructType:'结构模式需要 UScriptStruct 类型定义地址。可从成员的“展开此结构”自动进入。'});
Object.assign(strings.en,{followStruct:'→ Inspect struct',dereferencePointer:'Input is a pointer slot: dereference once',needStructType:'Struct mode requires a UScriptStruct type address. Use Inspect struct on a reflected member.'});
Object.assign(strings.zh,{declarationOnly:'类型声明（非实例值）',openDeclaredType:'→ 查看类型结构',fieldMetadata:'字段元数据'});
Object.assign(strings.en,{declarationOnly:'Type declaration (not a live value)',openDeclaredType:'→ Inspect type',fieldMetadata:'Field metadata'});
Object.assign(strings.zh,{classDeclarationHint:'当前查看类型声明：点击成员名或类型进入目标结构；字段地址不是运行时指针值。',openDefaultObject:'查看类默认对象（不是运行中的实例）'});
Object.assign(strings.en,{classDeclarationHint:'Viewing a type declaration: click the member or type to inspect its target layout. Field descriptor addresses are not live pointer values.',openDefaultObject:'Inspect class default object (not a runtime instance)'});
// ULevel.Actors viewer. The SDK inspector DOM is shared so pointers/types/history
// behave identically in both tabs; actor list polling has an independent cursor.
const worldBrowser={worlds:[],selected:'',level:'',pages:[],page:0,loading:false,sequence:0,
  controller:null,error:'',message:'',timer:null,live:false,levels:[],topology:'',generation:null,autoDiscoveryKey:''};
Object.assign(strings.zh,{worldObjects:'对象列表',discoverWorlds:'扫描 World',refreshActors:'刷新 Actors',
  worldSelect:'选择 World',worldLevel:'Level',liveActorList:'实时遍历',allLevels:'全部已加载 Level',
  worldNone:'未找到 World，请进入游戏场景后扫描。',worldScanning:'正在发现 UWorld',
  actorSlots:'Actor 槽位',loadedLevels:'已加载 Level',actorEmpty:'没有匹配的有效 Actor',
  worldScope:'UWorld → PersistentLevel / Levels → ULevel.Actors',worldAuto:'拥有 GameInstance',
  worldSelectHint:'选择 World 后遍历 Actors。',worldNeedsProbe:'先探测目标，再扫描世界对象。'});
Object.assign(strings.en,{worldObjects:'World objects',discoverWorlds:'Scan Worlds',refreshActors:'Refresh Actors',
  worldSelect:'World',worldLevel:'Level',liveActorList:'Live traversal',allLevels:'All loaded Levels',
  worldNone:'No World found. Enter a game scene and scan again.',worldScanning:'Discovering UWorld',
  actorSlots:'Actor slots',loadedLevels:'Loaded Levels',actorEmpty:'No matching valid Actor',
  worldScope:'UWorld → PersistentLevel / Levels → ULevel.Actors',worldAuto:'Has GameInstance',
  worldSelectHint:'Select a World to traverse its Actors.',worldNeedsProbe:'Probe a process before scanning Worlds.'});
function cancelWorldRequest(){worldBrowser.sequence++;worldBrowser.controller?.abort();worldBrowser.controller=null;worldBrowser.loading=false;}
function resetWorldBrowser(){
  cancelWorldRequest();clearTimeout(worldBrowser.timer);
  Object.assign(worldBrowser,{worlds:[],selected:'',level:'',pages:[],page:0,error:'',message:'',live:false,levels:[],topology:'',generation:null,autoDiscoveryKey:''});
  $('world-live').checked=false;renderWorldBrowser();
}
async function worldRequest(body,sequence){
  const controller=new AbortController();worldBrowser.controller=controller;
  const timer=setTimeout(()=>controller.abort(),12000);
  try{
    for(let attempt=0;attempt<20;attempt++){
    const response=await fetch('/api/explorer',{method:'POST',headers:{'Content-Type':'application/json','X-UMT-Client':'1'},body:JSON.stringify(body),signal:controller.signal,cache:'no-store'});
    const data=await response.json();
    if(sequence!==worldBrowser.sequence)throw new DOMException('Cancelled','AbortError');
    if(data.busy && attempt<19){await new Promise(resolve=>setTimeout(resolve,150));continue;}
    if(!response.ok || data.error){const error=new Error(data.error || 'World request failed');error.changed=!!data.changed;error.busy=!!data.busy;throw error;}
    return data;
    }
  }finally{clearTimeout(timer);if(worldBrowser.controller===controller)worldBrowser.controller=null;}
}
async function discoverWorlds(){
  if(!explorerReady() || worldBrowser.loading)return;
  cancelWorldRequest();const seq=worldBrowser.sequence,previous=worldBrowser.selected;
  worldBrowser.autoDiscoveryKey=String(state.pid)+':'+String(state.generation);
  worldBrowser.loading=true;worldBrowser.error='';worldBrowser.worlds=[];let cursor=0;
  try{
    while(seq===worldBrowser.sequence){
      worldBrowser.message=t('worldScanning')+' '+cursor;renderWorldBrowser();
      const page=await worldRequest({action:'worlds',cursor,limit:64,scanLimit:8192,generation:state.generation},seq);
      worldBrowser.generation=page.generation;
      worldBrowser.worlds.push(...page.items);
      if(page.complete)break;
      if(page.nextCursor<=cursor)throw new Error('World discovery cursor did not advance.');
      cursor=page.nextCursor;
      await new Promise(resolve=>setTimeout(resolve,10));
    }
    worldBrowser.worlds.sort((a,b)=>Number(b.hasGameInstance)-Number(a.hasGameInstance)||Number(b.levelCount)-Number(a.levelCount));
    worldBrowser.selected=worldBrowser.worlds.some(w=>w.address===previous)?previous:worldBrowser.worlds[0]?.address || '';
    worldBrowser.level='';worldBrowser.pages=[];worldBrowser.page=0;worldBrowser.message='';
  }catch(error){if(seq===worldBrowser.sequence)worldBrowser.error=error.name==='AbortError'?'Stopped':error.message;}
  finally{if(seq===worldBrowser.sequence){worldBrowser.loading=false;renderWorldBrowser();}}
  if(seq===worldBrowser.sequence && worldBrowser.selected)await loadActors('reset');
}
async function loadActors(mode='reset',retry=true){
  if(!explorerReady() || !worldBrowser.selected || worldBrowser.loading)return;
  cancelWorldRequest();const seq=worldBrowser.sequence;
  const old=worldBrowser.pages[worldBrowser.page];
  let pageIndex=mode==='next'?worldBrowser.page+1:mode==='refresh'?worldBrowser.page:0;
  let cursor=mode==='next'?(old?.nextCursor || 0):mode==='refresh'?(old?.startCursor || 0):0;
  const startCursor=cursor,seen=new Set(),limit=Number($('world-limit').value)||100;
  const query=$('world-query').value.trim();
  if(mode==='reset'){worldBrowser.pages=[];worldBrowser.page=0;worldBrowser.topology='';}
  if(mode==='refresh')worldBrowser.pages=worldBrowser.pages.slice(0,worldBrowser.page+1);
  const page={items:[],startCursor,nextCursor:cursor,totalSlots:0,complete:false,scanned:0,invalidSlots:0,warnings:[]};
  worldBrowser.loading=true;worldBrowser.error='';let restart=false;
  try{
    while(seq===worldBrowser.sequence && page.items.length<limit && !page.complete){
      const result=await worldRequest({action:'actors',worldAddress:worldBrowser.selected,...(worldBrowser.level?{levelAddress:worldBrowser.level}:{}),
        cursor,limit:limit-page.items.length,scanLimit:4096,query,generation:state.generation,
        ...(worldBrowser.topology?{expectedTopology:worldBrowser.topology}:{})},seq);
      worldBrowser.topology=result.topology;worldBrowser.levels=result.levels;
      for(const item of result.items)if(!seen.has(item.address)){seen.add(item.address);page.items.push(item);}
      page.nextCursor=result.nextCursor;page.totalSlots=result.totalSlots;page.complete=result.complete;
      page.scanned+=result.scanned;page.invalidSlots+=result.invalidSlots;page.warnings=[...new Set([...page.warnings,...result.warnings])];
      page.sampledAtMs=result.sampledAtMs;page.actorsOffset=result.actorsOffset;
      worldBrowser.pages[pageIndex]=page;worldBrowser.page=pageIndex;renderWorldBrowser();
      if(!page.complete && result.nextCursor<=cursor)throw new Error('Actor cursor did not advance.');
      cursor=result.nextCursor;
      if(!query)break;
      await new Promise(resolve=>setTimeout(resolve,10));
    }
  }catch(error){
    if(seq===worldBrowser.sequence){
      if(error.changed && retry){worldBrowser.topology='';restart=true;}
      else {worldBrowser.error=error.message;if(!error.busy){worldBrowser.live=false;$('world-live').checked=false;}}
    }
  }finally{if(seq===worldBrowser.sequence){worldBrowser.loading=false;renderWorldBrowser();}}
  if(restart)await loadActors('reset',false);
}
function renderWorldBrowser(){
  const ready=explorerReady(),w=worldBrowser,page=w.pages[w.page];
  const sessionKey=String(state.pid)+':'+String(state.generation);
  if(ready && activeTab==='world' && !w.worlds.length && !w.loading && w.autoDiscoveryKey!==sessionKey){w.autoDiscoveryKey=sessionKey;queueMicrotask(discoverWorlds);}
  $('world-error').hidden=!w.error;$('world-error').textContent=w.error;
  $('world-status').textContent=!ready?t('worldNeedsProbe'):w.message || t('worldScope');
  const choices=w.worlds.map(item=>{const option=node('option',item.name+' · '+item.address+(item.hasGameInstance?' · '+t('worldAuto'):''));option.value=item.address;return option;});
  if(!choices.length){const option=node('option',w.loading?t('worldScanning'):t('worldNone'));option.value='';choices.push(option);}
  const signature=JSON.stringify([choices.map(option=>[option.value,option.textContent]),language]);
  if($('world-select').dataset.signature!==signature){$('world-select').replaceChildren(...choices);$('world-select').dataset.signature=signature;}
  $('world-select').value=w.selected;
  const levelChoices=[node('option',t('allLevels'))];levelChoices[0].value='';
  for(const level of w.levels){const option=node('option',level.name+' ['+level.count+']');option.value=level.address;levelChoices.push(option);}
  const levelSig=JSON.stringify(levelChoices.map(option=>[option.value,option.textContent]));
  if($('world-level').dataset.signature!==levelSig){$('world-level').replaceChildren(...levelChoices);$('world-level').dataset.signature=levelSig;}
  $('world-level').value=w.level;
  $('world-discover').disabled=!ready || w.loading;$('world-refresh').disabled=!ready || w.loading || !w.selected;
  $('world-stop').hidden=!w.loading;$('world-select').disabled=!ready || w.loading;$('world-level').disabled=!ready || w.loading;
  $('world-prev').disabled=w.loading || !ready || w.page===0;
  $('world-next').disabled=w.loading || !ready || !page || page.complete;
  $('world-summary').textContent=page?t('actorSlots')+': '+page.totalSlots+' · '+t('loadedLevels')+': '+w.levels.length+' · '+t('matched')+': '+page.items.length+' · ULevel.Actors '+page.actorsOffset+' · '+new Date(page.sampledAtMs).toLocaleTimeString():t('worldSelectHint');
  if(page?.warnings.length)$('world-summary').textContent+=' · '+page.warnings.join(' ');
  $('world-page').textContent=page?(w.page+1)+' · '+page.nextCursor+'/'+page.totalSlots:'—';
  const actorSig=JSON.stringify([page?.items,explorer.detail?.address,ready,w.loading]);
  if($('world-actors').dataset.signature!==actorSig){
    $('world-actors').replaceChildren(...(page?.items || []).map(item=>{
      const button=node('button',undefined,'world-actor');button.type='button';button.disabled=!ready || w.loading;
      button.classList.toggle('selected',item.address===explorer.detail?.address);
      button.append(node('strong',item.name),node('span',item.className),node('code',item.address),
                    node('small',item.levelName+' · ['+item.actorSlot+']'));
      button.addEventListener('click',async()=>{await inspectObject(item.address,'instance');renderWorldBrowser();});
      return button;
    }));
    if(!page?.items.length)$('world-actors').append(node('div',t('actorEmpty'),'empty compact'));
    $('world-actors').dataset.signature=actorSig;
  }
}
function scheduleWorld(){
  clearTimeout(worldBrowser.timer);
  if(!worldBrowser.live)return;
  worldBrowser.timer=setTimeout(async()=>{
    if(worldBrowser.live && activeTab==='world' && !document.hidden && explorerReady() && !worldBrowser.loading && !explorer.loading)
      await loadActors('refresh');
    if(worldBrowser.live)scheduleWorld();
  },Math.max(1000,Number($('world-interval').value)||2000));
}
$('world-discover').addEventListener('click',discoverWorlds);
$('world-refresh').addEventListener('click',()=>loadActors('refresh'));
$('world-stop').addEventListener('click',()=>{cancelWorldRequest();worldBrowser.live=false;$('world-live').checked=false;clearTimeout(worldBrowser.timer);renderWorldBrowser();});
$('world-select').addEventListener('change',()=>{worldBrowser.selected=$('world-select').value;worldBrowser.level='';worldBrowser.levels=[];loadActors();});
$('world-level').addEventListener('change',()=>{worldBrowser.level=$('world-level').value;loadActors();});
$('world-search-form').addEventListener('submit',event=>{event.preventDefault();loadActors();});
$('world-limit').addEventListener('change',()=>loadActors());
$('world-prev').addEventListener('click',()=>{if(worldBrowser.page>0){worldBrowser.page--;loadActors('refresh');}});
$('world-next').addEventListener('click',()=>loadActors('next'));
$('world-live').addEventListener('change',()=>{worldBrowser.live=$('world-live').checked;scheduleWorld();});
$('world-interval').addEventListener('change',scheduleWorld);

Object.assign(strings.zh,{idaCanOpen:'IDA 已连接 · 支持打开库',idaCurrentOnly:'IDA 已连接 · 仅当前数据库',startIda:'保存并自动连接',startingIda:'正在启动…',startIdaHint:'电脑端自动启动已安装的 IDA MCP；缺依赖时显示安装指引。',idaStartupTimeout:'等待 IDA 启动超时。服务可能仍在启动，请点击“检查 IDA 连接”查看状态。'});
Object.assign(strings.en,{idaCanOpen:'IDA connected · database manager',idaCurrentOnly:'IDA connected · current database only',startIda:'Save & auto-connect',startingIda:'Starting…',startIdaHint:'Automatically starts the installed IDA MCP on this computer. Missing dependencies show installation instructions.',idaStartupTimeout:'Timed out waiting for IDA startup. The service may still be starting; use Check IDA connection to refresh its status.'});
$('ida-endpoint-form').addEventListener('submit',event=>{event.preventDefault();workbenchCommand('start_ida',{url:$('ida-endpoint').value.trim()});});
applyTheme(); translate(); poll(); refreshWorkbench(true);
