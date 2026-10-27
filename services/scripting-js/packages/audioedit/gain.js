// Clip gain in fixed steps.  args: selection, step (dB, default 1.0)
function setGain(ctx, compute) {
  let changed = 0;
  for (const clip of ctx.selectedClips()) {
    if (!clip.isAudio) continue;
    const before = clip.clipInfo.gainDb;
    const after = Math.max(-96, Math.min(24, compute(before)));
    if (after === before) continue;
    ctx.doc.execute(Host.Actions.SetProperty, { node: clip.id, key: 'gainDb', value: Host.Values.double(after) });
    ++changed;
  }
  return changed > 0;
}

function createGainCommands() {
  const hasAudio = (ctx) => ctx.selectedClips().some((c) => c.isAudio);
  return Host.Commands.handler([
    { category: 'Audio', name: 'Gain Up', title: 'Raise Clip Gain', enabled: hasAudio,
      run: (ctx) => setGain(ctx, (g) => g + Number(ctx.arg('step', 1.0))) },
    { category: 'Audio', name: 'Gain Down', title: 'Lower Clip Gain', enabled: hasAudio,
      run: (ctx) => setGain(ctx, (g) => g - Number(ctx.arg('step', 1.0))) },
    { category: 'Audio', name: 'Reset Gain', title: 'Reset Clip Gain', enabled: hasAudio,
      run: (ctx) => setGain(ctx, () => 0) },
  ]);
}
