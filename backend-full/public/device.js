const $ = (id) => document.getElementById(id);
let flashing = false;

async function api(path, opts = {}) {
  const response = await fetch(path, { headers: { 'Content-Type': 'application/json' }, ...opts });
  const data = await response.json();
  if (!response.ok || data.ok === false) throw new Error(data.message || data.error || 'Request failed');
  return data;
}
function showToast(message) {
  const toast = $('toast'); toast.textContent = message; toast.classList.remove('hidden');
  clearTimeout(showToast._timer); showToast._timer = setTimeout(() => toast.classList.add('hidden'), 3200);
}

function renderStatus(d) {
  const dot = document.querySelector('#bigStatus .dot');
  if (d.local_tools === false) {
    $('toolsBanner').classList.remove('hidden');
    $('toolsBanner').className = 'banner warn';
    $('toolsBanner').innerHTML = 'Local device tools are disabled. Start the backend with <code>LOCAL_DEVICE_TOOLS=1 npm start</code> on the machine the XIAO is plugged into.';
    dot.className = 'dot warn'; $('statusText').textContent = 'USB tools disabled';
    $('statusSub').textContent = 'Cloud mode'; return;
  }
  $('toolsBanner').classList.add('hidden');
  if (!d.connected) {
    dot.className = 'dot warn'; $('statusText').textContent = 'No XIAO detected on USB';
    $('statusSub').textContent = 'Plug in the XIAO with a data USB-C cable, then Refresh.';
    $('kvPort').textContent = '-'; $('kvFqbn').textContent = d.fqbn || '-'; $('kvRfid').textContent = '-'; $('kvUid').textContent = '-';
    return;
  }
  const rfid = d.rfid_ready === true ? 'Ready' : (d.rfid_ready === false ? 'Not answering' : 'Unknown (Test RFID)');
  dot.className = 'dot ok'; $('statusText').textContent = 'XIAO connected';
  $('statusSub').textContent = `On ${d.port}`;
  $('kvPort').textContent = d.port || '-'; $('kvFqbn').textContent = d.fqbn || '-';
  $('kvRfid').textContent = rfid; $('kvUid').textContent = d.last_uid || '-';
}

async function refresh(probe) {
  if (flashing) return;
  try { const d = await api('/api/device/status' + (probe ? '?probe=1' : '')); renderStatus(d); }
  catch (e) { showToast(e.message); }
}

async function flash() {
  if (flashing) return;
  flashing = true;
  const btn = $('flashBtn'); const label = btn.textContent; btn.disabled = true; btn.textContent = 'Flashing... (20-40s)';
  $('flashLogWrap').classList.remove('hidden'); $('flashLog').textContent = 'Compiling and uploading...\n';
  try {
    const response = await fetch('/api/device/flash', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
    const data = await response.json();
    $('flashLog').textContent = data.log || data.message || '(no output)';
    showToast(data.ok ? 'Firmware flashed successfully.' : 'Flash failed - see log.');
  } catch (e) {
    $('flashLog').textContent = e.message;
    showToast('Flash failed: ' + e.message);
  } finally {
    flashing = false; btn.disabled = false; btn.textContent = label;
    setTimeout(() => refresh(true), 1500);
  }
}

async function readCard() {
  const btn = $('readBtn'); const label = btn.textContent; btn.disabled = true; btn.textContent = 'Reading... place card';
  try {
    const res = await fetch('/api/cards/read', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
    const data = await res.json();
    if (data.present && data.uid) { $('readOut').textContent = JSON.stringify({ uid: data.uid, payload: data.payload, balance: data.balance }, null, 2); }
    else { $('readOut').textContent = data.message || 'No card detected.'; showToast(data.message || 'No card detected.'); }
  } catch (e) { showToast(e.message); $('readOut').textContent = e.message; }
  finally { btn.disabled = false; btn.textContent = label; }
}

$('refreshBtn').addEventListener('click', () => refresh(false));
$('probeBtn').addEventListener('click', () => { showToast('Probing RFID module...'); refresh(true); });
$('flashBtn').addEventListener('click', flash);
$('readBtn').addEventListener('click', readCard);

refresh(true);   // one probe on load so RFID status is definitive, not "Unknown"
setInterval(() => refresh(false), 5000);
