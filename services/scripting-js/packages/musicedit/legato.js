// Extends every note to the start of the next note that begins later (the last note keeps its
// length).  args: selection, gap (quarter notes left free before the next note, default 0)
Host.Package.include('common.js');

function createLegatoCommand() {
  return Host.Commands.handler({
    category: 'Music',
    name: 'Legato',
    title: 'Legato',
    enabled: (ctx) => MusicEdit.midiClips(ctx).length > 0,
    run(ctx) {
      const gap = Math.max(0, Number(ctx.arg('gap', 0)));
      let changed = 0;
      for (const clip of MusicEdit.midiClips(ctx)) {
        const ok = MusicEdit.rewrite(ctx, clip, (events) => {
          const notes = events.filter(Host.Midi.isNote).sort((a, b) => a.position - b.position);
          let any = false;
          for (let i = 0; i < notes.length; ++i) {
            const next = notes.find((n, j) => j > i && n.position > notes[i].position);
            if (!next) continue;
            const len = next.position - notes[i].position - gap;
            if (len > 0 && Math.abs(len - notes[i].length) > 1e-9) {
              notes[i].length = len;
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
