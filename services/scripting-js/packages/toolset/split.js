// Splits every selected clip that spans the position (clips of selected tracks included); locked
// clips are left alone.
//   args: selection, position (quarter notes, required)
function spanning(ctx, pos) {
  return ctx.selectedClips().filter((c) => !c.locked && c.start < pos && pos < c.end);
}

function createSplitCommand() {
  return Host.Commands.handler({
    category: 'Edit',
    name: 'Split At Position',
    title: 'Split Clips',
    enabled: (ctx) => typeof ctx.args.position === 'number' && spanning(ctx, ctx.args.position).length > 0,
    run(ctx) {
      const pos = ctx.args.position;
      for (const clip of spanning(ctx, pos)) ctx.doc.execute(Host.Actions.SplitClip, { node: clip.id, at: Host.Values.double(pos) });
    },
  });
}
