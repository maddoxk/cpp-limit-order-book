// UI for the WebAssembly order book demo. All matching happens in the C++
// engine (lob.wasm); this file only forwards user input and draws results.
'use strict';

const TICKS_PER_UNIT = 100;       // 1 tick = 0.01 in the UI
const LEVELS = 10;                // rows shown per side
const TAPE_MAX = 40;
const BENCH_MESSAGES = 200000;
const STATUS = ['Ok', 'Invalid id', 'Invalid price', 'Invalid quantity', 'Duplicate id', 'Unknown order'];

const $ = (id) => document.getElementById(id);
const fmtPrice = (ticks) => (ticks / TICKS_PER_UNIT).toFixed(2);
const fmtInt = (n) => Math.round(n).toLocaleString('en-US');

createLob().then((M) => {
  M._lob_reset(42);
  $('status').textContent = 'Engine ready (WebAssembly)';
  start(M);
}).catch((e) => {
  $('status').textContent = 'Failed to load the WebAssembly engine: ' + e;
});

function start(M) {
  let nextId = 1e9;                 // user ids stay far above generator ids
  const mine = new Map();           // id -> {side, price, size}
  let lastSeq = 0;
  let dirty = true;
  const tape = [];

  // ---- ladder DOM (created once, then updated in place so bars can animate)
  function makeRows(container, cls) {
    const rows = [];
    for (let i = 0; i < LEVELS; i++) {
      const row = document.createElement('div');
      row.className = 'row ' + cls + ' empty-row';
      row.innerHTML = '<div class="bar"></div><span></span><span class="num"></span><span class="num"></span>';
      row.addEventListener('click', () => {
        if (row.dataset.price) { $('price').value = row.dataset.price; $('type-limit').checked = true; syncType(); }
      });
      container.appendChild(row);
      rows.push(row);
    }
    return rows;
  }
  const askRows = makeRows($('asks'), 'ask');   // index 0 = best ask, drawn at the bottom
  askRows.reverse().forEach((r) => $('asks').appendChild(r));
  askRows.reverse();
  const bidRows = makeRows($('bids'), 'bid');

  function readSide(side, rows) {
    const n = M._lob_depth(side, LEVELS);
    const levels = [];
    for (let i = 0; i < n; i++) levels.push({ price: M._lob_out(i * 3), qty: M._lob_out(i * 3 + 1), orders: M._lob_out(i * 3 + 2) });
    const maxQty = Math.max(1, ...levels.map((l) => l.qty));
    const myPrices = new Set([...mine.values()].filter((o) => o.side === side).map((o) => o.price));
    rows.forEach((row, i) => {
      const l = levels[i];
      const cells = row.querySelectorAll('span');
      if (!l) {
        row.classList.add('empty-row'); row.classList.remove('mine'); delete row.dataset.price;
        row.firstChild.style.width = '0';
        cells[0].textContent = '-'; cells[1].textContent = '-'; cells[2].textContent = '-';
        return;
      }
      row.classList.remove('empty-row');
      row.classList.toggle('mine', myPrices.has(l.price));
      row.dataset.price = fmtPrice(l.price);
      row.firstChild.style.width = (100 * l.qty / maxQty).toFixed(1) + '%';
      cells[0].textContent = fmtPrice(l.price);
      cells[1].textContent = fmtInt(l.qty);
      cells[2].textContent = fmtInt(l.orders);
    });
  }

  function renderStats() {
    const bid = M._lob_stat(3), ask = M._lob_stat(4);
    if (bid && ask) {
      $('spread').innerHTML = 'Spread <strong>' + fmtPrice(ask - bid) + '</strong> &middot; Mid <strong>' + fmtPrice((ask + bid) / 2) + '</strong>';
    } else {
      $('spread').textContent = 'No two-sided market';
    }
    const rows = [
      ['Best bid', bid ? fmtPrice(bid) : '-'], ['Best ask', ask ? fmtPrice(ask) : '-'],
      ['Resting orders', fmtInt(M._lob_stat(2))],
      ['Levels (bid / ask)', fmtInt(M._lob_stat(5)) + ' / ' + fmtInt(M._lob_stat(6))],
      ['Trades', fmtInt(M._lob_stat(1))], ['Stream messages', fmtInt(M._lob_stat(0))],
      ['Stream rejects', fmtInt(M._lob_stat(7))],
    ];
    $('stats').innerHTML = rows.map(([k, v]) => '<dt>' + k + '</dt><dd>' + v + '</dd>').join('');
  }

  function renderTape() {
    const n = M._lob_trades_fetch(lastSeq, TAPE_MAX);
    if (!n) return;
    for (let i = 0; i < n; i++) {
      const o = i * 6;
      tape.unshift({ seq: M._lob_out(o), price: M._lob_out(o + 1), qty: M._lob_out(o + 2), side: M._lob_out(o + 3) });
      lastSeq = M._lob_out(o);
    }
    tape.length = Math.min(tape.length, TAPE_MAX);
    $('tape').innerHTML = tape.map((t, i) =>
      '<div class="trade ' + (t.side === 0 ? 'buy' : 'sell') + (i < n ? ' fresh' : '') + '"><span>' + fmtPrice(t.price) +
      '</span><span class="num">' + fmtInt(t.qty) + '</span><span class="num">' + fmtInt(t.seq) + '</span></div>').join('');
  }

  function renderMine() {
    for (const [id, o] of mine) {
      if (!M._lob_order(id)) { mine.delete(id); continue; }
      o.left = M._lob_out(2);
    }
    $('mine-empty').hidden = mine.size > 0;
    $('mine-body').innerHTML = [...mine].map(([id, o]) =>
      '<tr><td class="' + (o.side === 0 ? 'b' : 's') + '">' + (o.side === 0 ? 'Buy' : 'Sell') + '</td><td class="num">' + fmtPrice(o.price) +
      '</td><td class="num">' + fmtInt(o.left) + ' / ' + fmtInt(o.size) + '</td><td class="num"><button type="button" data-id="' + id + '">Cancel</button></td></tr>').join('');
  }

  function render() {
    renderMine();
    readSide(1, askRows);
    readSide(0, bidRows);
    renderStats();
    renderTape();
    dirty = false;
  }

  // ---- order entry
  function syncType() { $('price-field').hidden = $('type-market').checked; }
  document.querySelectorAll('input[name=type]').forEach((r) => r.addEventListener('change', syncType));
  syncType();

  $('order-form').addEventListener('submit', (ev) => {
    ev.preventDefault();
    $('form-error').textContent = '';
    const side = $('side-sell').checked ? 1 : 0;
    const market = $('type-market').checked;
    const qtyText = $('qty').value.trim();
    if (!/^\d+$/.test(qtyText) || +qtyText < 1 || +qtyText > 1000000) {
      $('form-error').textContent = 'Quantity must be a whole number from 1 to 1,000,000.'; return;
    }
    const qty = +qtyText;
    const id = nextId++;
    let status, desc;
    if (market) {
      status = M._lob_market(id, side, qty);
      desc = 'Market ' + (side ? 'sell' : 'buy') + ' ' + fmtInt(qty);
    } else {
      const priceText = $('price').value.trim();
      const price = Math.round(parseFloat(priceText) * TICKS_PER_UNIT);
      if (!/^\d*\.?\d+$/.test(priceText) || !(price >= 1) || price > 100000000) {
        $('form-error').textContent = 'Enter a price greater than 0 (for example 100.00).'; return;
      }
      status = M._lob_limit(id, side, price, qty);
      desc = 'Limit ' + (side ? 'sell' : 'buy') + ' ' + fmtInt(qty) + ' @ ' + fmtPrice(price);
      if (status === 0 && M._lob_last_resting() > 0) mine.set(id, { side, price, size: qty, left: M._lob_last_resting() });
    }
    if (status !== 0) { $('form-error').textContent = 'Rejected: ' + STATUS[status] + '.'; return; }
    const filled = M._lob_last_filled(), resting = M._lob_last_resting();
    $('result').textContent = desc + ': filled ' + fmtInt(filled) + ', resting ' + fmtInt(resting) +
      (market && filled < qty ? ', ' + fmtInt(qty - filled) + ' unfilled and discarded' : '') + '.';
    render();
  });

  $('mine-body').addEventListener('click', (ev) => {
    const id = ev.target.dataset && ev.target.dataset.id;
    if (!id) return;
    const status = M._lob_cancel(+id);
    mine.delete(+id);
    $('result').textContent = status === 0 ? 'Order cancelled.' : 'Order was already gone (' + STATUS[status] + ').';
    render();
  });

  $('reset').addEventListener('click', () => {
    M._lob_reset(42); mine.clear(); tape.length = 0; lastSeq = 0;
    $('tape').innerHTML = ''; $('result').textContent = 'Book reset.'; render();
  });

  // ---- streaming loop
  let last = performance.now(), lastRender = 0, carry = 0;
  function frame(now) {
    const dt = Math.min((now - last) / 1000, 0.25); last = now;
    if ($('stream').checked) {
      carry += Number($('rate').value) * dt;
      const n = Math.floor(carry);
      if (n > 0) { carry -= n; M._lob_stream_step(n); dirty = true; }
    }
    if (dirty && now - lastRender > 33) { lastRender = now; render(); }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);

  // ---- in-browser benchmark
  $('bench').addEventListener('click', () => {
    const btn = $('bench'); btn.disabled = true;
    const out = $('bench-out'); out.textContent = 'Running 3 x ' + fmtInt(BENCH_MESSAGES) + ' messages...';
    const rates = [];
    let run = 0;
    function once() {
      const ms = M._lob_bench(BENCH_MESSAGES, 42);
      rates.push(BENCH_MESSAGES / (ms / 1000));
      out.textContent = 'Run ' + (run + 1) + ' of 3: ' + (rates[run] / 1e6).toFixed(2) + ' M msgs/s...';
      if (++run < 3) { setTimeout(once, 30); return; }
      const sorted = [...rates].sort((a, b) => a - b);
      out.innerHTML = '<strong>' + (sorted[1] / 1e6).toFixed(2) + ' M msgs/s</strong> median of 3 runs (' +
        rates.map((r) => (r / 1e6).toFixed(2)).join(', ') + '). Measured in <em>your</em> browser, WebAssembly build, ' +
        fmtInt(BENCH_MESSAGES) + ' synthetic messages (' + fmtInt(M._lob_bench_stat(1)) + ' trades), apply loop only. ' +
        'Not comparable to the native numbers in the README, which come from a different machine.';
      btn.disabled = false;
    }
    setTimeout(once, 30);   // let the "running" text paint first
  });

  render();
}
