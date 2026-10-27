// Moves note starts of the selected MIDI clips toward the nearest grid line (song time).
//   args: selection, grid (quarter notes, default 0.25 = sixteenths), strength (0..1, default 1)
Host.Package.include('common.js');

function createQuantizeCommand() {
  return Host.Commands.handler({
    category: 'Music',
    name: 'Quantize',
    title: 'Quantize Note Starts',
    enabled: (ctx) => MusicEdit.midiClips(ctx).length > 0,
    run(ctx) {
      const grid = Number(ctx.arg('grid', 0.25));
      const strength = Math.max(0, Math.min(1, Number(ctx.arg('strength', 1))));
      if (!(grid > 0)) throw new Host.HostError('quantize: grid must be > 0', Host.Results.INVALIDARG);
      let changed = 0;
      for (const clip of MusicEdit.midiClips(ctx)) {
        const start = clip.start;
        const ok = MusicEdit.rewrite(ctx, clip, (events) => {
          let any = false;
          for (const e of events) {
            if (!Host.Midi.isNote(e)) continue;
            const abs = start + e.position;
            const target = Math.round(abs / grid) * grid;
            const pos = Math.max(0, abs + (target - abs) * strength - start);
            if (Math.abs(pos - e.position) > 1e-9) {
              e.position = pos;
              any = true;
            }
          }
          return any;
        });
        if (ok) ++changed;
      }
      return changed > 0;
    },
  });
}
