// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '../../..');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dxmt-capture-all-'));
function run(command, args) {
  const result = cp.spawnSync(command, args, {cwd: root, encoding:'utf8', timeout:60000});
  if (result.status !== 0) throw new Error(`${command}: ${result.error || result.stderr || result.stdout}`);
  return result.stdout;
}
try {
  const object = path.join(tmp, 'sha1.o'), exe = path.join(tmp, 'mock'), journal = path.join(tmp, 'capture.jsonl');
  run('clang', ['-fsanitize=address,undefined', '-c', 'src/util/sha1/sha1.c', '-o', object]);
  run('clang++', ['-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
    '-I','src/util','src/d3d12/tests/capture-all-mock.cpp','src/util/sha1/sha1_util.cpp',object,'-o',exe]);
  process.stdout.write(run(exe, [journal]));
  const text = fs.readFileSync(journal,'utf8');
  assert.ok(text.endsWith('\n'));
  const records = text.trimEnd().split('\n').map(JSON.parse);
  records.forEach((record,index) => { assert.equal(record.seq,index+1); assert.equal(record.v,1); assert.equal(record.pid,42); });
  assert.equal(records[0].event,'session.begin');
  assert.equal(records.at(-1).event,'session.end');
  assert.equal(records.at(-1).psos,2055);
  assert.equal(records.at(-1).unique_shaders,2055);
  assert.equal(records.at(-1).errors,0);
  assert.equal(records.at(-1).open_psos,0);
  assert.equal(records.at(-1).incomplete,false);
  for (let pso=1;pso<=2055;pso++) {
    const row=records.filter(r=>r.pso===pso);
    assert.deepEqual(row.map(r=>r.event),['pso.begin','shader.begin','shader.end','pso.result','pso.end']);
    const bytes=Buffer.alloc(4); bytes.writeUInt32LE(pso-1);
    assert.equal(row[2].sha1,require('node:crypto').createHash('sha1').update(bytes).digest('hex'));
    assert.equal(row[2].length,4);
    assert.equal(row[2].file,`capture-p42-0000000000000007-s${pso}.bin`);
    assert.equal(row[3].hr,'00000000');
  }
  console.log(`capture-all JSONL: ${records.length} valid ordered records, 2055 exact SHA1 associations PASS`);
} finally { fs.rmSync(tmp,{recursive:true,force:true}); }
