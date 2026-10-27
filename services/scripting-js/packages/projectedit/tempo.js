// Tempo map edits.
//   Set Tempo   args: bpm (required, 10..999), position (quarter notes, default 0), ramp?
//   Scale Tempo args: factor (default 2.0): multiplies every tempo point
function createTempoCommands() {
  const validBpm = (b) => typeof b === 'number' && b >= 10 && b <= 999;
  return Host.Commands.handler([
    {
      category: 'Project', name: 'Set Tempo', title: 'Set Tempo',
      enabled: (ctx) => validBpm(ctx.args.bpm),
      run(ctx) {
        ctx.doc.execute(Host.Actions.SetTempo, {
          position: Host.Values.double(Math.max(0, Number(ctx.arg('position', 0)))),
          bpm: Host.Values.double(ctx.args.bpm),
          ramp: !!ctx.arg('ramp', false),
        });
      },
    },
    {
      category: 'Project', name: 'Scale Tempo', title: 'Scale Tempo Map',
      enabled: (ctx) => Number(ctx.arg('factor', 2)) > 0 && ctx.doc.tempoMap !== null,
      run(ctx) {
        const factor = Number(ctx.arg('factor', 2));
        const points = ctx.doc.tempoMap.tempos;
        for (const p of points) {
          const bpm = Math.max(10, Math.min(999, p.bpm * factor));
          ctx.doc.execute(Host.Actions.SetTempo, {
            position: Host.Values.double(p.position),
            bpm: Host.Values.double(bpm),
            ramp: p.ramp,
          });
        }
        return points.length > 0;
      },
    },
  ]);
}
