// Short edge fades against clicks at clip boundaries.
//   args: selection, fadeIn / fadeOut (seconds, default 0.005)
function setFades(ctx, fadeIn, fadeOut) {
  let changed = 0;
  for (const clip of ctx.selectedClips()) {
    if (!clip.isAudio) continue;
    const info = clip.clipInfo;
    if (info.fadeIn !== fadeIn) {
      ctx.doc.execute(Host.Actions.SetProperty, { node: clip.id, key: 'fadeIn', value: Host.Values.double(fadeIn) });
      ++changed;
    }
    if (info.fadeOut !== fadeOut) {
      ctx.doc.execute(Host.Actions.SetProperty, { node: clip.id, key: 'fadeOut', value: Host.Values.double(fadeOut) });
      ++changed;
    }
  }
  return changed > 0;
}

function createFadeCommands() {
  const hasAudio = (ctx) => ctx.selectedClips().some((c) => c.isAudio);
  return Host.Commands.handler([
    { category: 'Audio', name: 'Apply Fades', title: 'Apply Edge Fades', enabled: hasAudio,
      run: (ctx) => setFades(ctx, Math.max(0, Number(ctx.arg('fadeIn', 0.005))), Math.max(0, Number(ctx.arg('fadeOut', 0.005)))) },
    { category: 'Audio', name: 'Remove Fades', title: 'Remove Fades', enabled: hasAudio,
      run: (ctx) => setFades(ctx, 0, 0) },
  ]);
}
