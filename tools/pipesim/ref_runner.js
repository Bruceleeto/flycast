// Run each corpus block through the reference simulator individually.
//
// Feeding it chunks was fast and silently wrong: it aborts a whole chunk on the
// first instruction it dislikes, so the result list came back short and every
// block after the first failure was compared against a different block's
// numbers. That turned a working model into an apparent 8% agreement rate.
//
// Per-block is slower but a failure stays contained, and blocks it cannot
// handle are reported as FAIL rather than shifting everything below them.
const fs = require('fs');
const HARNESS = process.env.SH4SIM ||
      '/home/bruce/Downloads/bloop/tools/SH-4-pipeline-simulator-harness-main';
const { analyze } = require(HARNESS + '/analyze.js');

const txt = fs.readFileSync(process.argv[2], 'utf8');
const blocks = txt.split(/^#.*$/m).filter(b => b.trim());

console.log('block\tref_cycles\tref_stalls');
blocks.forEach((b, i) => {
  const body = b.split('\n').map(s => s.trim()).filter(Boolean);
  try {
    const r = analyze('# b\n' + body.join('\n') + '\n');
    if (!r.length) { console.log(i + '\tFAIL\tno-result'); return; }
    const total = r[0].rows.reduce((a, x) => a + x.stall, 0);
    console.log(i + '\t' + r[0].cycles + '\t' + total);
  } catch (e) {
    console.log(i + '\tFAIL\t' + String(e.message).slice(0, 50).replace(/\s+/g, ' '));
  }
});
