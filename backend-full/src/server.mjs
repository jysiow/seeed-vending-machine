import express from 'express';
import cors from 'cors';
import path from 'path';
import { nanoid } from 'nanoid';
import { readCsv, writeCsv, appendCsv, nowIso } from './csvStore.mjs';
import * as device from './deviceTools.mjs';
import { readProducts, writeProducts, readCards, writeCards } from './dataMap.mjs';

const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());
app.use(express.json({ limit: '1mb' }));
app.use(express.static('public'));

const customerHeaders = ['user_name','total_paid','total_orders','last_order_number','created_at','updated_at'];
const orderHeaders = ['order_number','user_name','product_id','product_name','quantity','amount_paid','status','rfid_card_uid','written_payload','created_at','written_at','verified_at','dispensed_at','frontend_id','notes'];
const inventoryLogHeaders = ['time','action','product_id','product_name','quantity_delta','inventory_after','actor','notes'];
const writerJobHeaders = ['job_id','order_number','user_name','rfid_payload','rfid_card_uid','status','created_at','claimed_at','written_at','device_id','message','job_type'];
const deviceStatusHeaders = ['device_id','device_type','server_connected','rfid_ready','card_present','last_card_uid','current_job_id','last_seen_at','message'];
const cardLedgerHeaders = ['time','card_uid','user_name','type','amount','balance_after','actor','notes'];

const validOrderStartStates = new Set(['RFID_WRITTEN']);

function toNumber(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function requireDevice(req, res, next) {
  const deviceId = req.header('x-device-id');
  const apiKey = req.header('x-api-key');
  readCsv('devices.csv').then(devices => {
    const found = devices.find(d => d.device_id === deviceId && d.api_key === apiKey && d.enabled === 'true');
    if (!found) return res.status(401).json({ ok: false, error: 'Invalid device credentials' });
    req.device = found;
    next();
  }).catch(next);
}

// Gate for the local-only USB features (detect / flash / serial write).
function requireLocalTools(req, res, next) {
  if (!device.localToolsEnabled()) {
    return res.status(501).json({
      ok: false,
      error: 'local_device_tools_disabled',
      message: 'Set LOCAL_DEVICE_TOOLS=1 and run backend-full on the machine with the XIAO connected via USB.'
    });
  }
  next();
}

function parseBalanceFromPayload(payload) {
  if (!payload) return null;
  try {
    const obj = JSON.parse(payload);
    if (obj && obj.balance != null && Number.isFinite(Number(obj.balance))) return Number(obj.balance);
  } catch { /* not JSON, try regex */ }
  const m = /"balance"\s*:\s*(\d+(?:\.\d+)?)/.exec(payload);
  return m ? Number(m[1]) : null;
}

// Turn a serial write/read failure into an operator-friendly message.
function friendlyCardError(result) {
  const raw = result.raw || '';
  if (result.error === 'no_device') return 'No XIAO detected on USB. Open Device Setup to connect or flash.';
  if (/No card detected/i.test(raw)) return 'No card detected. Place the card flat on the reader and keep it there until it finishes.';
  if (/verify mismatch/i.test(raw)) return 'Card write could not be verified. Re-seat the card and try again.';
  if (result.error === 'parse_error') return 'The RFID writer did not respond. Re-flash the firmware from Device Setup, then retry.';
  return 'Card write failed. Use a MIFARE Classic 1K card, re-seat it, and retry.';
}

// Reserve stock and create an order row. Throws Error with .code for HTTP status.
async function createOrderRecord({ user_name, product_id, quantity, amount_paid, rfid_card_uid, status, notes }) {
  const qty = toNumber(quantity, 1);
  const rawProducts = await readProducts();
  const product = rawProducts.find(p => p.product_id === product_id);
  if (!product || String(product.active) !== 'true') { const e = new Error('product unavailable'); e.code = 404; throw e; }
  if (toNumber(product.inventory) < qty) { const e = new Error('not enough inventory'); e.code = 409; throw e; }

  product.inventory = String(toNumber(product.inventory) - qty);
  await writeProducts(rawProducts);

  const order_number = makeOrderNumber();
  const written_payload = makeWriterPayload(user_name, order_number);
  const orders = await readCsv('orders.csv');
  const newOrder = {
    order_number, user_name, product_id, product_name: product.product_name,
    quantity: String(qty), amount_paid: String(amount_paid ?? ''), status,
    rfid_card_uid: rfid_card_uid || '', written_payload, created_at: nowIso(),
    written_at: '', verified_at: '', dispensed_at: '', frontend_id: '', notes: notes || ''
  };
  orders.push(newOrder);
  await writeCsv('orders.csv', orders, orderHeaders);

  await appendCsv('inventory_log.csv', { time: nowIso(), action: 'RESERVE_FOR_ORDER', product_id: product.product_id, product_name: product.product_name, quantity_delta: -qty, inventory_after: product.inventory, actor: 'backend-center', notes: order_number }, inventoryLogHeaders);

  const customers = await readCsv('customers.csv');
  let customer = customers.find(c => c.user_name === user_name);
  if (!customer) { customer = { user_name, total_paid: '0', total_orders: '0', last_order_number: '', created_at: nowIso(), updated_at: nowIso() }; customers.push(customer); }
  customer.total_paid = String(toNumber(customer.total_paid) + toNumber(amount_paid));
  customer.total_orders = String(toNumber(customer.total_orders) + 1);
  customer.last_order_number = order_number;
  customer.updated_at = nowIso();
  await writeCsv('customers.csv', customers, customerHeaders);

  return { order: newOrder, product, written_payload };
}

async function getProducts() {
  const products = await readProducts();
  return products.map(p => ({
    ...p,
    price: toNumber(p.price),
    inventory: toNumber(p.inventory),
    low_stock_threshold: toNumber(p.low_stock_threshold),
    active: String(p.active) === 'true',
    is_low_stock: toNumber(p.inventory) <= toNumber(p.low_stock_threshold)
  }));
}

function makeOrderNumber() {
  const d = new Date();
  const stamp = d.toISOString().replace(/[-:.TZ]/g, '').slice(0, 14);
  return `ORD-${stamp}-${nanoid(6).toUpperCase()}`;
}

function makeWriterPayload(userName, orderNumber) {
  return JSON.stringify({ user_name: userName, order_number: orderNumber, v: 1 });
}

function summarizeMetrics(products, customers, orders, writerJobs, cards = []) {
  return {
    product_count: products.length,
    customer_count: customers.length,
    order_count: orders.length,
    pending_count: orders.filter(o => ['PENDING_WRITE','RFID_WRITTEN','VERIFIED'].includes(o.status)).length,
    completed_count: orders.filter(o => o.status === 'DISPENSED').length,
    low_stock_count: products.filter(p => p.is_low_stock).length,
    inventory_units_total: products.reduce((sum, p) => sum + p.inventory, 0),
    total_revenue: orders.reduce((sum, o) => sum + toNumber(o.amount_paid), 0),
    writer_pending_count: writerJobs.filter(j => ['PENDING','CLAIMED'].includes(j.status)).length,
    writer_written_count: writerJobs.filter(j => j.status === 'WRITTEN').length,
    card_count: cards.length,
    card_balance_total: cards.reduce((sum, c) => sum + toNumber(c.balance), 0)
  };
}

async function upsertDeviceStatus(update) {
  const statuses = await readCsv('device_status.csv');
  let status = statuses.find(s => s.device_id === update.device_id);
  if (!status) {
    status = { device_id: update.device_id, device_type: update.device_type || '', server_connected: '', rfid_ready: '', card_present: '', last_card_uid: '', current_job_id: '', last_seen_at: '', message: '' };
    statuses.push(status);
  }
  Object.assign(status, update, { last_seen_at: nowIso() });
  await writeCsv('device_status.csv', statuses, deviceStatusHeaders);
  return status;
}

app.get('/api/health', (req, res) => {
  res.json({ ok: true, service: 'backend-center', time: nowIso() });
});

app.get('/api/dashboard', async (req, res, next) => {
  try {
    const [products, customers, orders, inventory_logs, devices, writer_jobs, device_status, cards, card_ledger] = await Promise.all([
      getProducts(), readCsv('customers.csv'), readCsv('orders.csv'), readCsv('inventory_log.csv'), readCsv('devices.csv'), readCsv('writer_jobs.csv'), readCsv('device_status.csv'), readCards(), readCsv('card_ledger.csv')
    ]);
    const metrics = summarizeMetrics(products, customers, orders, writer_jobs, cards);
    res.json({
      ok: true,
      metrics,
      products,
      customers: [...customers].sort((a, b) => (b.updated_at || '').localeCompare(a.updated_at || '')),
      recent_orders: [...orders].reverse().slice(0, 25),
      low_stock: products.filter(p => p.is_low_stock),
      recent_inventory_logs: [...inventory_logs].reverse().slice(0, 20),
      devices,
      writer_jobs: [...writer_jobs].reverse().slice(0, 20),
      writer_status: device_status.find(s => s.device_id === 'wio-rfid-writer') || null,
      device_status,
      cards: [...cards].sort((a, b) => (b.updated_at || '').localeCompare(a.updated_at || '')),
      card_ledger: [...card_ledger].reverse().slice(0, 20),
      local_tools: device.localToolsEnabled()
    });
  } catch (err) { next(err); }
});

app.get('/api/products', async (req, res, next) => {
  try { res.json({ ok: true, products: await getProducts() }); } catch (err) { next(err); }
});

app.post('/api/products/:productId/refill', async (req, res, next) => {
  try {
    const qty = toNumber(req.body.quantity, 0);
    if (qty <= 0) return res.status(400).json({ ok: false, error: 'quantity must be positive' });
    const rawProducts = await readProducts();
    const product = rawProducts.find(p => p.product_id === req.params.productId);
    if (!product) return res.status(404).json({ ok: false, error: 'product not found' });
    product.inventory = String(toNumber(product.inventory) + qty);
    await writeProducts(rawProducts);
    await appendCsv('inventory_log.csv', { time: nowIso(), action: 'REFILL', product_id: product.product_id, product_name: product.product_name, quantity_delta: qty, inventory_after: product.inventory, actor: 'dashboard', notes: req.body.notes || '' }, inventoryLogHeaders);
    res.json({ ok: true, product });
  } catch (err) { next(err); }
});

app.post('/api/orders/create-and-prepare-card', async (req, res, next) => {
  try {
    const { user_name, product_id, quantity = 1, amount_paid = '', rfid_card_uid = '' } = req.body;
    if (!user_name || !product_id) return res.status(400).json({ ok: false, error: 'user_name and product_id are required' });
    if (toNumber(quantity, 1) <= 0) return res.status(400).json({ ok: false, error: 'quantity must be greater than 0' });

    const { order, written_payload } = await createOrderRecord({ user_name, product_id, quantity, amount_paid, rfid_card_uid, status: 'RFID_WRITTEN', notes: 'Waiting for Wio RFID writer' });

    const writerJobs = await readCsv('writer_jobs.csv');
    const writerJob = { job_id: `WJ-${nanoid(8).toUpperCase()}`, order_number: order.order_number, user_name, rfid_payload: written_payload, rfid_card_uid, status: 'PENDING', created_at: nowIso(), claimed_at: '', written_at: '', device_id: '', message: 'Waiting for card on Wio Terminal', job_type: 'ORDER' };
    writerJobs.push(writerJob);
    await writeCsv('writer_jobs.csv', writerJobs, writerJobHeaders);

    res.json({ ok: true, order, writer_job: writerJob, rfid_payload_to_write: written_payload, message: 'Order created. Wio RFID writer will write this when a card is present.' });
  } catch (err) { if (err.code) return res.status(err.code).json({ ok: false, error: err.message }); next(err); }
});

// Local USB flow: reserve stock, create order, and write the card synchronously.
app.post('/api/orders/create-and-write', requireLocalTools, async (req, res, next) => {
  try {
    const { user_name, product_id, quantity = 1, amount_paid = '' } = req.body;
    if (!user_name || !product_id) return res.status(400).json({ ok: false, error: 'user_name and product_id are required' });
    if (toNumber(quantity, 1) <= 0) return res.status(400).json({ ok: false, error: 'quantity must be greater than 0' });

    const { order, written_payload } = await createOrderRecord({ user_name, product_id, quantity, amount_paid, status: 'PENDING_WRITE', notes: 'Writing via USB' });

    const write = await device.serialWrite(written_payload);

    const orders = await readCsv('orders.csv');
    const ord = orders.find(o => o.order_number === order.order_number);
    if (ord) {
      if (write.ok) { ord.status = 'RFID_WRITTEN'; ord.written_at = nowIso(); ord.rfid_card_uid = write.uid || ''; ord.notes = 'RFID card written via USB'; }
      else { ord.status = 'WRITE_FAILED'; ord.notes = write.error || 'USB write failed'; }
      await writeCsv('orders.csv', orders, orderHeaders);
    }

    const writerJobs = await readCsv('writer_jobs.csv');
    const writerJob = { job_id: `WJ-${nanoid(8).toUpperCase()}`, order_number: order.order_number, user_name, rfid_payload: written_payload, rfid_card_uid: write.uid || '', status: write.ok ? 'WRITTEN' : 'FAILED', created_at: nowIso(), claimed_at: nowIso(), written_at: write.ok ? nowIso() : '', device_id: 'xiao-usb-writer', message: write.ok ? 'Written via USB' : (write.error || 'USB write failed'), job_type: 'ORDER' };
    writerJobs.push(writerJob);
    await writeCsv('writer_jobs.csv', writerJobs, writerJobHeaders);

    if (!write.ok) return res.status(502).json({ ok: false, error: 'card_write_failed', message: friendlyCardError(write), detail: write.error || '', order: ord, writer_job: writerJob, log: write.raw || '' });
    res.json({ ok: true, order: ord, writer_job: writerJob, rfid_card_uid: write.uid || '', payload: JSON.parse(written_payload), message: 'Order created and RFID card written.' });
  } catch (err) { if (err.code) return res.status(err.code).json({ ok: false, error: err.message }); next(err); }
});

// Local USB flow: write a stored-value balance to the card + record the ledger.
app.post('/api/cards/topup-and-write', requireLocalTools, async (req, res, next) => {
  try {
    const { user_name, amount, mode = 'add', card_uid = '' } = req.body;
    const amt = toNumber(amount, NaN);
    if (!user_name) return res.status(400).json({ ok: false, error: 'user_name is required' });
    if (!Number.isFinite(amt) || amt < 0) return res.status(400).json({ ok: false, error: 'amount must be a non-negative number' });
    if (!['add', 'set'].includes(mode)) return res.status(400).json({ ok: false, error: 'mode must be add or set' });

    const cards = await readCards();
    let uid = String(card_uid || '').trim();
    let currentBalance = 0;

    if (mode === 'add') {
      if (uid) {
        const existing = cards.find(c => c.card_uid === uid);
        currentBalance = existing ? toNumber(existing.balance) : 0;
      } else {
        const read = await device.serialRead();
        if (!read.ok || !read.uid) return res.status(502).json({ ok: false, error: 'card_read_failed', message: friendlyCardError(read), detail: read.error || 'no card detected', log: read.raw || '' });
        uid = read.uid;
        const onCard = parseBalanceFromPayload(read.payload);
        const existing = cards.find(c => c.card_uid === uid);
        currentBalance = onCard != null ? onCard : (existing ? toNumber(existing.balance) : 0);
      }
    }

    const newBalance = mode === 'set' ? amt : currentBalance + amt;
    const payload = JSON.stringify({ user_name, balance: newBalance, v: 1 });
    if (payload.length > 96) return res.status(400).json({ ok: false, error: 'payload_too_long', message: 'Shorten user_name; only 96 bytes fit on the card.' });

    const write = await device.serialWrite(payload);
    if (!write.ok) return res.status(502).json({ ok: false, error: 'card_write_failed', message: friendlyCardError(write), detail: write.error || '', log: write.raw || '' });
    uid = write.uid || uid || 'UNKNOWN';

    let card = cards.find(c => c.card_uid === uid);
    if (!card) { card = { card_uid: uid, user_name, mode: 'DIRECT', balance: '0', status: 'ACTIVE', created_at: nowIso(), updated_at: '', last_payload: '', notes: '' }; cards.push(card); }
    card.user_name = user_name;
    card.balance = String(newBalance);
    card.status = 'ACTIVE';
    card.updated_at = nowIso();
    card.last_payload = payload;
    await writeCards(cards);

    await appendCsv('card_ledger.csv', { time: nowIso(), card_uid: uid, user_name, type: mode === 'set' ? 'SET' : 'TOPUP', amount: String(amt), balance_after: String(newBalance), actor: 'operator', notes: '' }, cardLedgerHeaders);

    res.json({ ok: true, card, rfid_card_uid: uid, balance: newBalance, payload: JSON.parse(payload), message: 'Balance written to card.' });
  } catch (err) { if (err.code) return res.status(err.code).json({ ok: false, error: err.message }); next(err); }
});

// ---- Local device (USB) endpoints ----
app.get('/api/device/status', async (req, res, next) => {
  try {
    if (!device.localToolsEnabled()) return res.json({ ok: true, local_tools: false, connected: false, message: 'Local device tools disabled' });
    const status = await device.getStatus({ probe: req.query.probe === '1' });
    res.json({ ok: true, local_tools: true, ...status });
  } catch (err) { next(err); }
});

app.post('/api/device/flash', requireLocalTools, async (req, res, next) => {
  try {
    const result = await device.flash();
    res.status(result.ok ? 200 : 500).json({ ok: result.ok, log: result.log, port: result.port || null });
  } catch (err) { next(err); }
});

app.post('/api/cards/read', requireLocalTools, async (req, res, next) => {
  try {
    const port = await device.detectPort({ fresh: true });
    if (!port) return res.json({ ok: false, error: 'no_device', message: 'No XIAO detected on USB. Open Device Setup to connect or flash.' });
    const read = await device.serialRead(port);
    if (read.ok && read.uid) {
      return res.json({ ok: true, present: true, uid: read.uid, payload: read.payload || null, balance: parseBalanceFromPayload(read.payload) });
    }
    if (/No card detected/i.test(read.raw || '')) {
      return res.json({ ok: true, present: false, uid: null, message: 'No card detected. Place the card flat on the reader and keep it there, then Read again.' });
    }
    return res.json({ ok: false, error: read.error || 'read_failed', message: friendlyCardError(read), raw: read.raw || '' });
  } catch (err) { next(err); }
});

app.post('/api/rfid-writer/status', requireDevice, async (req, res, next) => {
  try {
    if (req.device.device_type !== 'writer') return res.status(403).json({ ok: false, error: 'device is not a writer' });
    const status = await upsertDeviceStatus({
      device_id: req.device.device_id,
      device_type: 'writer',
      server_connected: 'true',
      rfid_ready: String(Boolean(req.body.rfid_ready)),
      card_present: String(Boolean(req.body.card_present)),
      last_card_uid: req.body.last_card_uid || '',
      current_job_id: req.body.current_job_id || '',
      message: req.body.message || ''
    });
    res.json({ ok: true, status, server_time: nowIso() });
  } catch (err) { next(err); }
});

app.get('/api/rfid-writer/next-job', requireDevice, async (req, res, next) => {
  try {
    if (req.device.device_type !== 'writer') return res.status(403).json({ ok: false, error: 'device is not a writer' });
    const jobs = await readCsv('writer_jobs.csv');
    let job = jobs.find(j => j.status === 'CLAIMED' && j.device_id === req.device.device_id) || jobs.find(j => j.status === 'PENDING');
    if (!job) return res.json({ ok: true, has_job: false, message: 'No pending RFID write job' });
    if (job.status === 'PENDING') {
      job.status = 'CLAIMED';
      job.claimed_at = nowIso();
      job.device_id = req.device.device_id;
      job.message = 'Claimed by Wio RFID writer';
      await writeCsv('writer_jobs.csv', jobs, writerJobHeaders);
    }
    res.json({ ok: true, has_job: true, job });
  } catch (err) { next(err); }
});

app.post('/api/rfid-writer/job-result', requireDevice, async (req, res, next) => {
  try {
    if (req.device.device_type !== 'writer') return res.status(403).json({ ok: false, error: 'device is not a writer' });
    const { job_id, success, rfid_card_uid = '', message = '' } = req.body;
    const jobs = await readCsv('writer_jobs.csv');
    const job = jobs.find(j => j.job_id === job_id);
    if (!job) return res.status(404).json({ ok: false, error: 'writer job not found' });
    job.status = success ? 'WRITTEN' : 'FAILED';
    job.written_at = success ? nowIso() : '';
    job.device_id = req.device.device_id;
    job.rfid_card_uid = rfid_card_uid || job.rfid_card_uid;
    job.message = message || (success ? 'RFID card written successfully' : 'RFID card write failed');
    await writeCsv('writer_jobs.csv', jobs, writerJobHeaders);

    const orders = await readCsv('orders.csv');
    const order = orders.find(o => o.order_number === job.order_number);
    if (order) {
      if (success) {
        order.written_at = nowIso();
        order.rfid_card_uid = rfid_card_uid || order.rfid_card_uid;
        order.notes = 'RFID card written by Wio Terminal';
      } else {
        order.notes = `RFID writer failed: ${job.message}`;
      }
      await writeCsv('orders.csv', orders, orderHeaders);
    }
    await upsertDeviceStatus({ device_id: req.device.device_id, device_type: 'writer', server_connected: 'true', rfid_ready: 'true', card_present: rfid_card_uid ? 'true' : '', last_card_uid: rfid_card_uid, current_job_id: '', message: job.message });
    res.json({ ok: true, job, order });
  } catch (err) { next(err); }
});

app.post('/api/frontend/verify-card', requireDevice, async (req, res, next) => {
  try {
    const { user_name, order_number, rfid_card_uid = '' } = req.body;
    const orders = await readCsv('orders.csv');
    const order = orders.find(o => o.user_name === user_name && o.order_number === order_number);
    if (!order) return res.status(404).json({ ok: false, allow_dispense: false, error: 'order not found' });
    if (order.status === 'DISPENSED') return res.status(409).json({ ok: false, allow_dispense: false, error: 'order already used' });
    if (!validOrderStartStates.has(order.status)) return res.status(409).json({ ok: false, allow_dispense: false, error: `invalid order status: ${order.status}` });
    order.status = 'VERIFIED';
    order.verified_at = nowIso();
    order.frontend_id = req.device.device_id;
    if (rfid_card_uid) order.rfid_card_uid = rfid_card_uid;
    await writeCsv('orders.csv', orders, orderHeaders);
    const products = await readProducts();
    const product = products.find(p => p.product_id === order.product_id);
    res.json({ ok: true, allow_dispense: true, order_number: order.order_number, product_id: order.product_id, product_name: order.product_name, quantity: toNumber(order.quantity, 1), slot_id: product?.slot_id || '', servo_id: product?.servo_id || '', message: 'Card verified. Dispense the reserved item now.' });
  } catch (err) { next(err); }
});

app.post('/api/frontend/dispense-complete', requireDevice, async (req, res, next) => {
  try {
    const { order_number, success, notes = '' } = req.body;
    const orders = await readCsv('orders.csv');
    const order = orders.find(o => o.order_number === order_number);
    if (!order) return res.status(404).json({ ok: false, error: 'order not found' });
    order.status = success ? 'DISPENSED' : 'DISPENSE_FAILED';
    order.dispensed_at = nowIso();
    order.frontend_id = req.device.device_id;
    order.notes = notes;
    await writeCsv('orders.csv', orders, orderHeaders);
    res.json({ ok: true, order, message: success ? 'Order completed.' : 'Dispense failed. Operator review needed.' });
  } catch (err) { next(err); }
});

app.post('/api/orders/:orderNumber/cancel', async (req, res, next) => {
  try {
    const orders = await readCsv('orders.csv');
    const order = orders.find(o => o.order_number === req.params.orderNumber);
    if (!order) return res.status(404).json({ ok: false, error: 'order not found' });
    if (order.status === 'DISPENSED') return res.status(409).json({ ok: false, error: 'cannot cancel dispensed order' });
    if (order.status !== 'CANCELLED') {
      const rawProducts = await readProducts();
      const product = rawProducts.find(p => p.product_id === order.product_id);
      if (product) {
        product.inventory = String(toNumber(product.inventory) + toNumber(order.quantity));
        await writeProducts(rawProducts);
        await appendCsv('inventory_log.csv', { time: nowIso(), action: 'CANCEL_RELEASE_STOCK', product_id: product.product_id, product_name: product.product_name, quantity_delta: toNumber(order.quantity), inventory_after: product.inventory, actor: 'dashboard', notes: req.params.orderNumber }, inventoryLogHeaders);
      }
      order.status = 'CANCELLED';
      order.notes = `${order.notes || ''}${order.notes ? ' | ' : ''}Cancelled from dashboard`;
      await writeCsv('orders.csv', orders, orderHeaders);
      const jobs = await readCsv('writer_jobs.csv');
      let changed = false;
      for (const job of jobs) {
        if (job.order_number === order.order_number && ['PENDING','CLAIMED','FAILED'].includes(job.status)) { job.status = 'CANCELLED'; job.message = 'Order cancelled'; changed = true; }
      }
      if (changed) await writeCsv('writer_jobs.csv', jobs, writerJobHeaders);
    }
    res.json({ ok: true, order });
  } catch (err) { next(err); }
});

app.use((req, res) => { res.sendFile(path.resolve('public/index.html')); });
app.use((err, req, res, next) => { console.error(err); res.status(500).json({ ok: false, error: err.message }); });
app.listen(PORT, () => { console.log(`backend-center running at http://localhost:${PORT}`); });
