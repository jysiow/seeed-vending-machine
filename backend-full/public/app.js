const state = { dashboard: null, selectedProductId: null, mode: 'product', device: null };
const $ = (id) => document.getElementById(id);

async function api(path, opts = {}) {
  const response = await fetch(path, { headers: { 'Content-Type': 'application/json' }, ...opts });
  const data = await response.json();
  if (!response.ok || data.ok === false) throw new Error(data.message || data.error || data.detail || 'Request failed');
  return data;
}
function money(value) { return `$${Number(value || 0).toFixed(2)}`; }
function statusClass(status) { return `<span class="status ${status}">${status}</span>`; }
function stockBadge(product) {
  if (product.inventory <= 0) return '<span class="stock-pill out">Out of stock</span>';
  if (product.inventory <= product.low_stock_threshold) return '<span class="stock-pill low">Low stock</span>';
  return '<span class="stock-pill ok">In stock</span>';
}
function showToast(message) {
  const toast = $('toast'); toast.textContent = message; toast.classList.remove('hidden');
  clearTimeout(showToast._timer); showToast._timer = setTimeout(() => toast.classList.add('hidden'), 3200);
}
function productById(id) { return state.dashboard?.products?.find(item => item.product_id === id) || null; }
function tableHead(cols) { return '<tr>' + cols.map(col => `<th>${col}</th>`).join('') + '</tr>'; }

// ---------------- Device banner ----------------
function usbReady() { return state.device && state.device.local_tools && state.device.connected; }

function renderDeviceBanner() {
  const d = state.device;
  const banner = $('deviceBanner');
  if (!d || d.local_tools === false) {
    banner.className = 'banner info';
    banner.innerHTML = 'Cloud mode: USB flashing/writing is disabled. Cards are written by the WiFi writer. Run locally with <code>LOCAL_DEVICE_TOOLS=1</code> to enable USB.';
  } else if (!d.connected) {
    banner.className = 'banner warn';
    banner.innerHTML = 'No RFID writer detected on USB. <a class="text-link" href="/device.html">Open Device Setup</a> to connect and flash the firmware.';
  } else {
    const rfid = d.rfid_ready === true ? ' - RFID module ready' : (d.rfid_ready === false ? ' - RFID module not answering (flash firmware?)' : '');
    banner.className = 'banner ok';
    banner.innerHTML = `RFID writer connected on <strong>${d.port}</strong>${rfid}. <a class="text-link" href="/device.html">Device Setup</a>`;
  }
  applyUsbButtons();
}

function applyUsbButtons() {
  const ready = usbReady();
  const cloud = state.device && state.device.local_tools === false;
  [['createOrderWriteButton'], ['writeBalanceButton'], ['readCardButton']].forEach(([id]) => {
    const btn = $(id); if (!btn) return;
    btn.disabled = !ready;
    btn.title = ready ? '' : (cloud ? 'USB tools disabled (cloud mode)' : 'Connect the XIAO on USB first');
  });
  const queueBtn = $('createOrderQueueButton');
  if (queueBtn) queueBtn.style.display = cloud ? 'inline-flex' : (ready ? 'none' : 'inline-flex');
}

let devicePolling = false;
async function pollDevice() {
  if (devicePolling) return;
  devicePolling = true;
  try { const d = await api('/api/device/status'); state.device = d; }
  catch { state.device = { local_tools: false, connected: false }; }
  finally { devicePolling = false; }
  renderDeviceBanner();
}

// ---------------- Metrics ----------------
function renderMetrics() {
  const m = state.dashboard.metrics;
  const cards = [
    ['Products', m.product_count, 'Configured SKUs'], ['Customers', m.customer_count, 'Unique user_name records'],
    ['Orders', m.order_count, 'All orders created'], ['Pending', m.pending_count, 'Awaiting use / dispense'],
    ['Cards', m.card_count ?? 0, 'Stored-value cards'], ['Card Balance', money(m.card_balance_total), 'Total on all cards'],
    ['Units Left', m.inventory_units_total, 'All physical stock'], ['Revenue', money(m.total_revenue), 'Paid amount recorded']
  ];
  $('metrics').innerHTML = cards.map(([label, value, sub]) => `<article class="metric-card"><div class="metric-label">${label}</div><div class="metric-value">${value}</div><div class="metric-sub">${sub}</div></article>`).join('');
}

// ---------------- Mode ----------------
function setMode(mode) {
  state.mode = mode;
  [...document.querySelectorAll('.mode-tab')].forEach(t => t.classList.toggle('active', t.dataset.mode === mode));
  $('productPane').classList.toggle('hidden', mode !== 'product');
  $('balancePane').classList.toggle('hidden', mode !== 'balance');
}

// ---------------- Products ----------------
function renderProducts() {
  const products = state.dashboard.products;
  $('productGrid').innerHTML = products.map(product => `<article class="product-card ${product.product_id === state.selectedProductId ? 'selected' : ''}" data-product-id="${product.product_id}"><div class="product-image-wrap"><img src="${product.image_path}" alt="${product.product_name}" /></div><div class="product-card-body"><div class="card-head"><div><div class="tag">${product.tag}</div><h3>${product.product_name}</h3></div><div class="price">${money(product.price)}</div></div><div class="stock-row">${stockBadge(product)}<span>${product.inventory} left - slot ${product.slot_id}</span></div></div></article>`).join('');
  [...document.querySelectorAll('.product-card')].forEach(card => card.addEventListener('click', () => { state.selectedProductId = card.dataset.productId; renderProducts(); renderSelectedProduct(); }));
}
function renderSelectedProduct() {
  const product = productById(state.selectedProductId); const box = $('selectedProductCard');
  if (!product) { box.className = 'selected-card empty-state'; box.innerHTML = 'Select a product card to begin.'; return; }
  box.className = 'selected-card'; $('amount_paid').placeholder = `Recommended: ${money(product.price)}`;
  box.innerHTML = `<img src="${product.image_path}" alt="${product.product_name}" /><div><div class="tag">Selected</div><h3>${product.product_name}</h3><div class="stock-row">${stockBadge(product)}<span>${product.inventory} left - ${money(product.price)}</span></div></div>`;
}

// ---------------- Result box ----------------
function renderResult(title, obj, steps) {
  $('payloadOutput').textContent = `${title}\n` + JSON.stringify(obj, null, 2);
  $('writerSteps').innerHTML = (steps || []).map(s => `<li>${s}</li>`).join('');
}

// ---------------- Actions: product ----------------
function productBody() {
  return { user_name: $('user_name').value.trim(), product_id: state.selectedProductId, quantity: $('quantity').value, amount_paid: $('amount_paid').value, rfid_card_uid: $('rfid_card_uid').value.trim() };
}
function resetProductForm() { $('user_name').value = ''; $('quantity').value = '1'; $('amount_paid').value = ''; $('rfid_card_uid').value = ''; }

async function createOrderWrite() {
  if (!state.selectedProductId) return showToast('Select a product first.');
  const body = productBody();
  if (!body.user_name) return showToast('user_name is required.');
  const btn = $('createOrderWriteButton'); const label = btn.textContent; btn.disabled = true; btn.textContent = 'Writing card... place card on reader';
  try {
    const data = await api('/api/orders/create-and-write', { method: 'POST', body: JSON.stringify(body) });
    $('rfid_card_uid').value = data.rfid_card_uid || '';
    renderResult('Order written to card', { order_number: data.order.order_number, product: data.order.product_name, card_uid: data.rfid_card_uid, payload: data.payload }, ['Stock reserved and order created.', 'Payload written to the card over USB.', 'Read-back verified. Order is redeemable at the machine.']);
    showToast(`Card written for ${data.order.order_number}.`); resetProductForm(); await load();
  } catch (e) { showToast(e.message); renderResult('Write failed', { error: e.message }, ['Check the card is on the reader and is MIFARE Classic 1K.']); }
  finally { btn.textContent = label; applyUsbButtons(); }
}

async function createOrderQueue() {
  if (!state.selectedProductId) return showToast('Select a product first.');
  const body = productBody();
  if (!body.user_name) return showToast('user_name is required.');
  try {
    const data = await api('/api/orders/create-and-prepare-card', { method: 'POST', body: JSON.stringify(body) });
    renderResult('Order queued for WiFi writer', { order_number: data.order.order_number, writer_job: data.writer_job.job_id, payload: JSON.parse(data.rfid_payload_to_write) }, ['Order created and stock reserved.', 'WiFi writer will poll and write when a card is present.']);
    showToast('Order queued for the WiFi writer.'); resetProductForm(); await load();
  } catch (e) { showToast(e.message); }
}

// ---------------- Actions: balance ----------------
async function readCard() {
  const btn = $('readCardButton'); const label = btn.textContent; btn.disabled = true; btn.textContent = 'Reading... place card';
  const info = $('balanceReadInfo');
  try {
    const res = await fetch('/api/cards/read', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
    const data = await res.json();
    if (data.present && data.uid) {
      $('bal_card_uid').value = data.uid;
      info.classList.remove('hidden');
      info.innerHTML = `Card <strong>${data.uid}</strong> - current balance on card: <strong>${data.balance != null ? money(data.balance) : 'none'}</strong>`;
      showToast(`Card ${data.uid} read.`);
    } else {
      info.classList.remove('hidden');
      info.innerHTML = data.message || 'No card detected.';
      showToast(data.message || 'No card detected.');
    }
  } catch (e) { showToast('Read failed: ' + e.message); }
  finally { btn.textContent = label; applyUsbButtons(); }
}

async function writeBalance() {
  const body = { user_name: $('bal_user_name').value.trim(), amount: $('bal_amount').value, mode: $('bal_mode').value, card_uid: $('bal_card_uid').value.trim() };
  if (!body.user_name) return showToast('user_name is required.');
  if (!(Number(body.amount) >= 0)) return showToast('Enter a valid amount.');
  const btn = $('writeBalanceButton'); const label = btn.textContent; btn.disabled = true; btn.textContent = 'Writing balance... place card';
  try {
    const data = await api('/api/cards/topup-and-write', { method: 'POST', body: JSON.stringify(body) });
    $('bal_card_uid').value = data.rfid_card_uid || '';
    renderResult('Balance written to card', { card_uid: data.rfid_card_uid, user_name: data.card.user_name, new_balance: data.balance, payload: data.payload }, ['Balance written to the card over USB.', 'Read-back verified.', 'Ledger + card record updated.']);
    showToast(`Balance ${money(data.balance)} written to ${data.rfid_card_uid}.`); $('bal_amount').value = ''; await load();
  } catch (e) { showToast(e.message); renderResult('Balance write failed', { error: e.message }, ['Check the card is on the reader.']); }
  finally { btn.textContent = label; applyUsbButtons(); }
}

// ---------------- Tables ----------------
function renderWriterPanel() {
  const s = state.dashboard.writer_status;
  const jobs = state.dashboard.writer_jobs || [];
  const connected = s && s.server_connected === 'true';
  const ready = s && s.rfid_ready === 'true';
  const card = s && s.card_present === 'true';
  $('writerStatusCards').innerHTML = [
    ['Server link', connected ? 'Connected' : 'Waiting', connected ? 'ok' : 'low'],
    ['RFID module', ready ? 'Ready' : 'Not ready', ready ? 'ok' : 'low'],
    ['Card present', card ? 'Detected' : 'No card', card ? 'ok' : 'low'],
    ['Last card UID', s?.last_card_uid || '-', 'ok']
  ].map(([label, value, tone]) => `<div class="mini-status"><div class="hint">${label}</div><strong class="${tone}">${value}</strong></div>`).join('');
  $('writerMessage').textContent = s?.message || 'USB writer reports after each write; the WiFi writer polls the API.';
  $('writerJobsTable').innerHTML = tableHead(['Job', 'Type', 'Order', 'User', 'Status', 'Device', 'Message']) + jobs.map(j => `<tr><td>${j.job_id}</td><td>${j.job_type || 'ORDER'}</td><td>${j.order_number}</td><td>${j.user_name}</td><td>${statusClass(j.status)}</td><td>${j.device_id || '-'}</td><td>${j.message || ''}</td></tr>`).join('');
}
function renderCardsTable() {
  const cards = state.dashboard.cards || [];
  $('cardsTable').innerHTML = tableHead(['Card UID', 'User', 'Balance', 'Status', 'Updated']) + (cards.length ? cards.map(c => `<tr><td><strong>${c.card_uid}</strong></td><td>${c.user_name || '-'}</td><td>${money(c.balance)}</td><td>${statusClass(c.status || 'ACTIVE')}</td><td>${c.updated_at || ''}</td></tr>`).join('') : `<tr><td colspan="5" class="hint">No stored-value cards yet.</td></tr>`);
}
function renderCardLedgerTable() {
  const rows = state.dashboard.card_ledger || [];
  $('cardLedgerTable').innerHTML = tableHead(['Time', 'Card UID', 'User', 'Type', 'Amount', 'Balance After']) + (rows.length ? rows.map(r => `<tr><td>${r.time || ''}</td><td>${r.card_uid || ''}</td><td>${r.user_name || ''}</td><td>${r.type || ''}</td><td>${money(r.amount)}</td><td>${money(r.balance_after)}</td></tr>`).join('') : `<tr><td colspan="6" class="hint">No balance activity yet.</td></tr>`);
}
function renderInventoryTable() {
  const q = $('inventorySearch').value.trim().toLowerCase(); const products = state.dashboard.products.filter(p => !q || p.product_name.toLowerCase().includes(q));
  $('inventoryTable').innerHTML = tableHead(['Product','Price','Inventory','Threshold','Slot','Servo','Refill','Page']) + products.map(product => `<tr><td><strong>${product.product_name}</strong><div class="hint">${product.tag}</div></td><td>${money(product.price)}</td><td>${stockBadge(product)}<div class="hint">${product.inventory} units</div></td><td>${product.low_stock_threshold}</td><td>${product.slot_id}</td><td>${product.servo_id}</td><td><div class="inline-controls"><input class="small-input" id="refill_${product.product_id}" type="number" min="1" value="1" /><button data-refill-id="${product.product_id}">Refill</button></div></td><td><a class="text-link" href="${product.product_url}" target="_blank" rel="noreferrer">Open</a></td></tr>`).join('');
  [...document.querySelectorAll('[data-refill-id]')].forEach(btn => btn.addEventListener('click', async () => { const id = btn.dataset.refillId; const quantity = Number(document.getElementById(`refill_${id}`).value || 0); if (quantity <= 0) return showToast('Refill quantity must be greater than 0.'); try { await api(`/api/products/${id}/refill`, { method: 'POST', body: JSON.stringify({ quantity }) }); showToast('Inventory refilled successfully.'); await load(); } catch (e) { showToast(e.message); } }));
}
function renderOrdersTable() {
  const q = $('orderSearch').value.trim().toLowerCase(); const orders = state.dashboard.recent_orders.filter(o => !q || [o.order_number,o.user_name,o.product_name].some(v => String(v).toLowerCase().includes(q)));
  $('ordersTable').innerHTML = tableHead(['Order','User','Product','Qty','Status','Created','Actions']) + orders.map(order => `<tr><td><strong>${order.order_number}</strong></td><td>${order.user_name}</td><td>${order.product_name}</td><td>${order.quantity}</td><td>${statusClass(order.status)}</td><td>${order.created_at || ''}</td><td>${['DISPENSED','CANCELLED'].includes(order.status) ? '-' : `<button data-cancel-id="${order.order_number}">Cancel</button>`}</td></tr>`).join('');
  [...document.querySelectorAll('[data-cancel-id]')].forEach(btn => btn.addEventListener('click', async () => { if (!confirm(`Cancel order ${btn.dataset.cancelId}? Reserved inventory will be released.`)) return; try { await api(`/api/orders/${btn.dataset.cancelId}/cancel`, { method: 'POST', body: '{}' }); showToast('Order cancelled and inventory released.'); await load(); } catch (e) { showToast(e.message); } }));
}
function renderCustomersTable() { const c = state.dashboard.customers.slice(0,20); $('customersTable').innerHTML = tableHead(['User Name','Total Paid','Orders','Last Order','Updated']) + c.map(x => `<tr><td><strong>${x.user_name}</strong></td><td>${money(x.total_paid)}</td><td>${x.total_orders}</td><td>${x.last_order_number || '-'}</td><td>${x.updated_at || ''}</td></tr>`).join(''); }
function renderInventoryLogsTable() { const logs = state.dashboard.recent_inventory_logs; $('inventoryLogTable').innerHTML = tableHead(['Time','Action','Product','Delta','Inventory After','Actor','Notes']) + logs.map(log => `<tr><td>${log.time || ''}</td><td>${log.action || ''}</td><td>${log.product_name || ''}</td><td>${log.quantity_delta || ''}</td><td>${log.inventory_after || ''}</td><td>${log.actor || ''}</td><td>${log.notes || ''}</td></tr>`).join(''); }
function renderDevicesTable() { const d = state.dashboard.device_status.length ? state.dashboard.device_status : state.dashboard.devices; $('devicesTable').innerHTML = tableHead(['Device ID','Type','Server','RFID','Card','Last Seen','Message']) + d.map(x => `<tr><td><strong>${x.device_id}</strong></td><td>${x.device_type}</td><td>${x.server_connected || '-'}</td><td>${x.rfid_ready || '-'}</td><td>${x.card_present || '-'}</td><td>${x.last_seen_at || '-'}</td><td>${x.message || x.notes || ''}</td></tr>`).join(''); }

function renderQuickAmounts() {
  $('quickAmounts').innerHTML = [10, 20, 50, 100].map(v => `<button class="secondary chip" data-amount="${v}">+${money(v)}</button>`).join('');
  [...document.querySelectorAll('#quickAmounts [data-amount]')].forEach(b => b.addEventListener('click', () => { $('bal_amount').value = b.dataset.amount; }));
}

async function load() {
  const data = await api('/api/dashboard'); state.dashboard = data;
  if (!state.selectedProductId && data.products.length) state.selectedProductId = data.products[0].product_id;
  if (!productById(state.selectedProductId) && data.products.length) state.selectedProductId = data.products[0].product_id;
  renderMetrics(); renderProducts(); renderSelectedProduct(); renderWriterPanel(); renderCardsTable(); renderCardLedgerTable(); renderInventoryTable(); renderOrdersTable(); renderCustomersTable(); renderInventoryLogsTable(); renderDevicesTable();
}

// ---------------- Wire up ----------------
$('refreshButton').addEventListener('click', () => { load().catch(e => showToast(e.message)); pollDevice(); });
[...document.querySelectorAll('.mode-tab')].forEach(t => t.addEventListener('click', () => setMode(t.dataset.mode)));
$('createOrderWriteButton').addEventListener('click', createOrderWrite);
$('createOrderQueueButton').addEventListener('click', createOrderQueue);
$('writeBalanceButton').addEventListener('click', writeBalance);
$('readCardButton').addEventListener('click', readCard);
$('inventorySearch').addEventListener('input', renderInventoryTable);
$('orderSearch').addEventListener('input', renderOrdersTable);

renderQuickAmounts();
load().catch(e => showToast(e.message));
pollDevice();
setInterval(() => load().catch(() => {}), 6000);
setInterval(pollDevice, 4000);
