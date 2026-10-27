// Moves the selected clips by a fixed amount.
//   args: selection, amount (quarter notes, default 0.25). Clips never move before the song start;
//   locked clips never move.
function movable(ctx) {
  return ctx.selectedClips().filter((c) => !c.locked);
}

function nudge(ctx, direction) {
  const amount = Math.abs(Number(ctx.arg('amount', 0.25)));
  if (!(amount > 0)) return false;
  let moved = 0;
  for (const clip of movable(ctx)) {
    const start = clip.start;
    const target = Math.max(0, start + direction * amount);
    if (target === start) continue;
    ctx.doc.execute(Host.Actions.MoveClip, { node: clip.id, start: Host.Values.double(target) });
    ++moved;
  }
  return moved > 0;
}

function createNudgeCommands() {
  const hasClips = (ctx) => movable(ctx).length > 0;
  return Host.Commands.handler([
    { category: 'Edit', name: 'Nudge Left', title: 'Nudge Left', enabled: hasClips, run: (ctx) => nudge(ctx, -1) },
    { category: 'Edit', name: 'Nudge Right', title: 'Nudge Right', enabled: hasClips, run: (ctx) => nudge(ctx, +1) },
  ]);
}
