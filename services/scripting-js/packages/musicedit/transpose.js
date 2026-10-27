// Transpose the notes of the selected MIDI clips.  args: selection, semitones (Transpose, default 1)
Host.Package.include('common.js');

function transpose(ctx, semitones) {
  let changed = 0;
  for (const clip of MusicEdit.midiClips(ctx)) {
    const ok = MusicEdit.rewrite(ctx, clip, (events) => {
      let any = false;
      for (const e of events) {
        if (!Host.Midi.isNote(e)) continue;
        const key = Math.max(0, Math.min(127, e.data1 + semitones));
        if (key !== e.data1) {
          e.data1 = key;
          any = true;
        }
      }
      return any;
    });
    if (ok) ++changed;
  }
  return changed > 0;
}

function createTransposeCommands() {
  const hasMidi = (ctx) => MusicEdit.midiClips(ctx).length > 0;
  return Host.Commands.handler([
    { category: 'Music', name: 'Transpose', title: 'Transpose Notes', enabled: hasMidi,
      run: (ctx) => transpose(ctx, Math.trunc(Number(ctx.arg('semitones', 1)))) },
    { category: 'Music', name: 'Octave Up', title: 'Octave Up', enabled: hasMidi, run: (ctx) => transpose(ctx, 12) },
    { category: 'Music', name: 'Octave Down', title: 'Octave Down', enabled: hasMidi, run: (ctx) => transpose(ctx, -12) },
  ]);
}
