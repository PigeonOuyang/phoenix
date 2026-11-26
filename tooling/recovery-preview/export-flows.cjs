// SPDX-License-Identifier: Unlicense
// Export only the approved design's synthetic scenarios, never an installation log.
'use strict';

const fs = require('node:fs');
const vm = require('node:vm');

if (process.argv.length !== 3) {
  console.error('usage: node export-flows.cjs APPROVED_FLOWS_JS');
  process.exit(2);
}
const filename = process.argv[2];
const window = {};
vm.runInNewContext(fs.readFileSync(filename, 'utf8'), {window}, {filename, timeout: 1000});
const {get, caps, tr} = window.RecoveryFlows;
const routes = Object.entries(caps).flatMap(([mode, choices]) =>
  choices.map(([scenario]) => {
    const flow = get(mode, scenario);
    const stages = flow.stages.map((stage, i) => {
      const idleTail = mode === 'headset' && scenario === 'download-error' && stage.id === 'build'
        ? get(mode, 'fresh').stages[i].phases.filter(p => !stage.phases.some(q => q.id === p.id))
        : [];
      return {
        id: stage.id,
        label: stage.label,
        weight: stage.weight,
        phases: [...stage.phases, ...idleTail].map(p => ({
          id: p.id,
          label: p.label,
          weight: p.weight,
          outcome: p.outcome || '',
          ...(p.note ? {note: p.note} : {}),
          ...(p.meter ? {meter: p.meter} : {})
        }))
      };
    });
    const segments = flow.segments.map(({kind, i, j, from, to, result}) =>
      ({kind, i: i ?? -1, j: j ?? -1, from, to, result: result || ''}));
    return {mode, scenario, stages, segments, duration: flow.duration};
  }));
process.stdout.write(JSON.stringify({routes, idle: tr.idleMsg, complete: tr.doneMsg, handoff: tr.holdStatus}));
