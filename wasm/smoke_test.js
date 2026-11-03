// Headless smoke test of the WebAssembly module under Node.
//   node wasm/smoke_test.js path/to/lob.js
const assert = require('assert');
const path = require('path');

const modPath = path.resolve(process.argv[2] || 'build-wasm/wasm_out/lob.js');
const createLob = require(modPath);

const OK = 0, UNKNOWN_ID = 5, BUY = 0, SELL = 1;

createLob().then((M) => {
  // 1. Crossing orders produce a trade.
  M._lob_reset(1);
  assert.strictEqual(M._lob_limit(1, SELL, 10000, 10), OK);
  assert.strictEqual(M._lob_limit(2, BUY, 10000, 4), OK);
  assert.strictEqual(M._lob_last_filled(), 4);
  assert.strictEqual(M._lob_last_resting(), 0);
  assert.strictEqual(M._lob_trades_fetch(0, 10), 1);
  assert.strictEqual(M._lob_out(1), 10000); // price
  assert.strictEqual(M._lob_out(2), 4);     // qty
  assert.strictEqual(M._lob_out(4), 1);     // maker id
  assert.strictEqual(M._lob_out(5), 2);     // taker id
  assert.strictEqual(M._lob_stat(1), 1);    // trade count

  // 2. Depth shows the remaining 6 on the ask; cancel removes it.
  assert.strictEqual(M._lob_depth(SELL, 5), 1);
  assert.strictEqual(M._lob_out(0), 10000);
  assert.strictEqual(M._lob_out(1), 6);
  assert.strictEqual(M._lob_cancel(1), OK);
  assert.strictEqual(M._lob_depth(SELL, 5), 0);
  assert.strictEqual(M._lob_cancel(1), UNKNOWN_ID);
  assert.strictEqual(M._lob_order(1), 0);

  // 3. Modify (reduce keeps the order, reprice moves it) and market order.
  assert.strictEqual(M._lob_limit(3, BUY, 9990, 10), OK);
  assert.strictEqual(M._lob_modify(3, 9990, 5), OK);
  assert.strictEqual(M._lob_order(3), 1);
  assert.strictEqual(M._lob_out(2), 5);
  assert.strictEqual(M._lob_modify(3, 9995, 5), OK);
  assert.strictEqual(M._lob_stat(3), 9995); // best bid
  assert.strictEqual(M._lob_market(4, SELL, 100), OK);
  assert.strictEqual(M._lob_last_filled(), 5);
  assert.strictEqual(M._lob_stat(2), 0);    // book empty again

  // 4. Validation errors come back as status codes.
  assert.strictEqual(M._lob_limit(5, BUY, 0, 1), 2);  // InvalidPrice
  assert.strictEqual(M._lob_limit(5, BUY, 100, 0), 3); // InvalidQty

  // 5. Synthetic stream drives the same book.
  M._lob_reset(42);
  M._lob_stream_step(5000);
  assert.strictEqual(M._lob_stat(0), 5000);
  assert.ok(M._lob_stat(2) > 0, 'book has resting orders');
  assert.ok(M._lob_depth(BUY, 10) > 0);

  // 6. Replay returns the right count and matches the native build exactly:
  //    `lob_gen --seed 42 --count 200000` + `lob_replay` printed 68882 trades
  //    and trade checksum ab0eb8050ca899b8 in CI (low 32 bits: 0ca899b8).
  const ms = M._lob_bench(200000, 42);
  assert.ok(ms > 0);
  assert.strictEqual(M._lob_bench_stat(0), 200000);
  assert.strictEqual(M._lob_bench_stat(2), 0, 'no rejects');
  assert.strictEqual(M._lob_bench_stat(1), 68882);
  assert.strictEqual(M._lob_bench_stat(3), 0x0ca899b8);
  assert.strictEqual(M._lob_bench_stat(4), 159);

  console.log(`wasm smoke test passed (200k-message replay: ${ms.toFixed(1)} ms in Node)`);
}).catch((e) => { console.error(e); process.exit(1); });
